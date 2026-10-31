/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeldOperands.h"
#include "AOTUseEffectHistory.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffects.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSort.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned bitsPerWord = 64;

static size_t wordIndexOf(ObjectID object) { return object.index() / bitsPerWord; }
static uint64_t maskOf(ObjectID object) { return static_cast<uint64_t>(1) << (object.index() % bitsPerWord); }

static bool mergeInto(std::span<uint64_t> head, std::span<const uint64_t> tail)
{
    uint64_t added = 0;
    for (size_t word = 0; word < head.size(); ++word) {
        added |= tail[word] & ~head[word];
        head[word] |= tail[word];
    }
    return !!added;
}

static std::optional<uint32_t> actionIndexOf(const Function& function, const UseLists& useLists, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return useLists.locationOf(use.user).index;
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return pointBeforeTerminatorOf(function, use.block).index;
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasKnownHistory(const TrackedObject& object, ObjectUses objectUses, OperandUses requestedUses)
{
    OperandUses listedUses;
    bool hasEscapingUse = false;
    for (auto& objectUse : objectUses) {
        listedUses.add(objectUse.uses);
        hasEscapingUse |= objectUse.isEscape;
    }
    switch (object.state) {
    case EscapeState::Unknown:
        return false;
    case EscapeState::Escapes:
        if (!hasEscapingUse)
            return false;
        break;
    case EscapeState::DoesNotEscape:
        break;
    }
    return listedUses.contains(OperandUse::Retains) || listedUses.containsAll(object.uses & requestedUses);
}

std::optional<ValueID> findFreshEntryParameter(const Function& function, const CFG& cfg)
{
    if (function.role != FunctionRole::CoroutineBody)
        return std::nullopt;
    const CreatorRecord::Coroutine* record = function.creator.tryAs<CreatorRecord::Coroutine>();
    if (!record || !record->promiseParameter)
        return std::nullopt;
    auto& parameters = function.at(cfg.root()).parameters;
    if (*record->promiseParameter >= parameters.size())
        return std::nullopt;
    return parameters[*record->promiseParameter];
}

UseEffectHistory::UseEffectHistory(FunctionAnalyses& analyses, const UseEffectHistoryOptions& options)
    : m_function(analyses.function())
    , m_cfg(analyses.cfg())
    , m_numberOfObjects(analyses.escape().numberOfObjects())
{
    const EscapeAnalysis& escape = analyses.escapeWithLiveInstances();
    if (escape.gaveUp()) {
        m_gaveUp = true;
        return;
    }

    uint64_t numberOfBlocks = m_cfg.reversePostOrder().size();
    if (analyses.context().build.exceeds(LimitID::MaxUseEffectHistoryBits, numberOfBlocks * m_numberOfObjects, analyses.subject())) {
        m_gaveUp = true;
        return;
    }
    if (!m_numberOfObjects)
        return;

    m_wordsPerBlock = (m_numberOfObjects + bitsPerWord - 1) / bitsPerWord;
    m_head.fill(0, numberOfBlocks * m_wordsPerBlock);
    m_objectsFromOutside.fill(0, m_wordsPerBlock);
    m_isSkipped.ensureSize(m_cfg.numNodes());
    for (unsigned block = 0; block < m_cfg.numNodes(); ++block) {
        bool isReachable = m_cfg.isReachable(BlockID(block));
        if (isReachable && !(options.ignoredBlocks && options.ignoredBlocks->get(block)))
            continue;
        m_isSkipped.quickSet(block);
        m_skipsReachableBlocks |= isReachable;
    }
    m_isUnknown.ensureSize(m_numberOfObjects);
    for (unsigned objectIndex = 0; objectIndex < m_numberOfObjects; ++objectIndex) {
        if (!hasKnownHistory(escape.at(ObjectID(objectIndex)), escape.usesOf(ObjectID(objectIndex)), options.uses))
            m_isUnknown.quickSet(objectIndex);
    }

    struct PendingAction {
        uint32_t blockOrder;
        Action action;
    };

    const UseLists& useLists = analyses.useLists();
    std::optional<ValueID> freshEntryParameter = findFreshEntryParameter(m_function, m_cfg);
    OperandUses eventUses = options.uses;
    if (!options.uses.isEmpty())
        eventUses.add(OperandUse::Retains);

    // A saved frame is out of the program's reach. So is what lies in a list or an object that has gone nowhere but into saved frames.
    auto isHeldBySuspension = [&](const Use& use) {
        return use.kind == Use::Kind::Operand && isHeldOperand(m_function.at(use.user), use.operandIndex);
    };
    BitVector leavesOnlyIntoSavedFrames(m_numberOfObjects);
    auto goesIntoSavedFramesAlone = [&](const ObjectUse& objectUse) {
        if (isHeldBySuspension(objectUse.use))
            return true;
        if (!objectUse.retainedBy)
            return false;
        PointsToSet containers = escape.pointsTo(objectUse.retainedBy);
        if (containers.mayBeUntracked)
            return false;
        for (ObjectID container : containers.objects) {
            if (!leavesOnlyIntoSavedFrames.quickGet(container.index()))
                return false;
        }
        return true;
    };
    bool savedFramesAreOutOfReach = options.uses.isEmpty() && analyses.context().savedFrames == SavedFrames::AreOutOfReach;
    if (savedFramesAreOutOfReach) {
        for (unsigned objectIndex = 0; objectIndex < m_numberOfObjects; ++objectIndex) {
            if (escape.at(ObjectID(objectIndex)).kind == ObjectKind::Allocation && !m_isUnknown.quickGet(objectIndex))
                leavesOnlyIntoSavedFrames.quickSet(objectIndex);
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t objectIndex : BitVector(leavesOnlyIntoSavedFrames)) {
                for (auto& objectUse : escape.usesOf(ObjectID(objectIndex))) {
                    if (!objectUse.isEscape || goesIntoSavedFramesAlone(objectUse))
                        continue;
                    leavesOnlyIntoSavedFrames.quickClear(objectIndex);
                    changed = true;
                    break;
                }
            }
        }
    }
    Vector<PendingAction> pendingActions;
    auto addAction = [&](BlockID block, uint32_t index, ObjectID object, ActionKind kind) {
        pendingActions.append(PendingAction { m_cfg.reversePostOrderIndex(block), Action { index, object, kind } });
    };
    auto addEventAtHead = [&](BlockID block, ObjectID object) { mutableHeadOf(block)[wordIndexOf(object)] |= maskOf(object); };

    for (unsigned objectIndex = 0; objectIndex < m_numberOfObjects; ++objectIndex) {
        if (m_isUnknown.quickGet(objectIndex))
            continue;
        ObjectID object(objectIndex);
        auto& tracked = escape.at(object);
        bool isFreshAtEntry = tracked.definition == freshEntryParameter;
        switch (tracked.kind) {
        case ObjectKind::EntryParameter:
            if (isFreshAtEntry)
                break;
            addEventAtHead(m_cfg.root(), object);
            m_objectsFromOutside[wordIndexOf(object)] |= maskOf(object);
            continue;
        case ObjectKind::Allocation:
            break;
        }

        for (BlockID block : isFreshAtEntry ? m_cfg.reversePostOrder() : std::span<const BlockID>()) {
            auto& values = m_function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (analyses.maySuspend(values[index]) && !isSkipped(block))
                    addAction(block, index, object, ActionKind::Event);
            }
        }

        for (auto& objectUse : escape.usesOf(object)) {
            auto& use = objectUse.use;
            if ((!objectUse.isEscape && !objectUse.uses.containsAny(eventUses)) || isSkipped(use.block))
                continue;
            if (savedFramesAreOutOfReach && goesIntoSavedFramesAlone(objectUse))
                continue;
            if (std::optional<uint32_t> index = actionIndexOf(m_function, useLists, use))
                addAction(use.block, *index, object, ActionKind::Event);
            else
                addEventAtHead(use.block, object);
        }

        ValueLocation allocation = useLists.locationOf(tracked.definition);
        if (allocation.placement == ValuePlacement::Parameter) {
            const BitVector& renewed = escape.objectsRenewedAtMakersOf(object);
            for (BlockID maker : blocksWithResultIn(m_function, m_cfg, allocation.block, tracked.definition)) {
                if (isSkipped(maker))
                    continue;
                for (size_t index : renewed) {
                    if (escape.at(ObjectID(index)).kind == ObjectKind::Allocation && !m_isUnknown.quickGet(index))
                        addAction(maker, pointBeforeTerminatorOf(m_function, maker).index, ObjectID(index), ActionKind::Reset);
                }
            }
            continue;
        }
        if (escape.liveInstancesOf(object) != LiveInstances::AtMostOne || allocation.placement != ValuePlacement::InBlock || isSkipped(allocation.block))
            continue;
        addAction(allocation.block, allocation.index, object, ActionKind::Reset);
    }

    AOT::sortInLine(pendingActions, { }, [](const PendingAction& pending) {
        auto& action = pending.action;
        return std::tuple { pending.blockOrder, action.object, action.index, action.kind == ActionKind::Event };
    });

    m_firstAction.fill(0, numberOfBlocks + 1);
    m_actions.reserveInitialCapacity(pendingActions.size());
    for (auto& pending : pendingActions) {
        ++m_firstAction[pending.blockOrder + 1];
        m_actions.append(pending.action);
    }
    for (uint32_t blockOrder = 0; blockOrder < numberOfBlocks; ++blockOrder)
        m_firstAction[blockOrder + 1] += m_firstAction[blockOrder];

    if (!analyses.context().build.options().verifiesAfterEveryPass) [[likely]] {
        computeHeads(StartsAt::BlocksWhereSomethingHappens);
        return;
    }
    Vector<uint64_t> headsAtStart = m_head;
    computeHeads(StartsAt::BlocksWhereSomethingHappens);
    Vector<uint64_t> heads = std::exchange(m_head, WTF::move(headsAtStart));
    computeHeads(StartsAt::EveryBlock);
    RELEASE_ASSERT(m_head == heads);
}

void UseEffectHistory::computeStateLeaving(BlockID block, Leaves leaves, std::span<uint64_t> state) const
{
    memcpySpan(state, headOf(block));
    for (auto& action : actionsOf(block)) {
        uint64_t& word = state[wordIndexOf(action.object)];
        switch (action.kind) {
        case ActionKind::Event:
            word |= maskOf(action.object);
            break;
        case ActionKind::Reset:
            if (leaves == Leaves::AtEnd)
                word &= ~maskOf(action.object);
            break;
        }
    }
}

void UseEffectHistory::computeHeads(StartsAt startsAt)
{
    auto order = m_cfg.reversePostOrder();
    if (!m_skipsReachableBlocks && startsAt == StartsAt::BlocksWhereSomethingHappens) {
        Vector<uint64_t, 8> happenedAtEntry(m_wordsPerBlock, [&](size_t word) { return headOf(m_cfg.root())[word] & m_objectsFromOutside[word]; });
        bool hasAny = std::ranges::any_of(happenedAtEntry, [](uint64_t word) { return !!word; });
        for (uint32_t blockOrder = 0; hasAny && blockOrder < order.size(); ++blockOrder)
            mergeInto(mutableHeadOf(order[blockOrder]), happenedAtEntry.span());
    }

    BitVector isPending(order.size());
    for (uint32_t blockOrder = 0; blockOrder < order.size(); ++blockOrder) {
        BlockID block = order[blockOrder];
        if (isSkipped(block))
            continue;
        std::span<const uint64_t> head = headOf(block);
        bool hasMoreAtHead = false;
        for (size_t word = 0; word < head.size(); ++word)
            hasMoreAtHead |= !!(m_skipsReachableBlocks ? head[word] : head[word] & ~m_objectsFromOutside[word]);
        if (startsAt == StartsAt::EveryBlock || hasMoreAtHead || std::ranges::any_of(actionsOf(block), [](const Action& action) { return action.kind == ActionKind::Event; }))
            isPending.quickSet(blockOrder);
    }

    Vector<uint64_t, 8> tail;
    Vector<uint64_t, 8> stateAtHandler;
    tail.fill(0, m_wordsPerBlock);
    stateAtHandler.fill(0, m_wordsPerBlock);
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t blockOrder = 0; blockOrder < order.size(); ++blockOrder) {
            if (!isPending.quickClear(blockOrder))
                continue;
            BlockID block = order[blockOrder];
            computeStateLeaving(block, Leaves::AtEnd, tail.mutableSpan());
            if (m_cfg.hasHandler(block))
                computeStateLeaving(block, Leaves::ToHandler, stateAtHandler.mutableSpan());

            auto successors = m_cfg.successors(block);
            for (uint32_t successorIndex = 0; successorIndex < successors.size(); ++successorIndex) {
                BlockID successor = successors[successorIndex];
                if (isSkipped(successor))
                    continue;
                bool isHandlerEdge = outgoingEdgeAt(m_function, block, successorIndex).isHandlerEdge();
                if (!mergeInto(mutableHeadOf(successor), isHandlerEdge ? stateAtHandler.span() : tail.span()))
                    continue;
                uint32_t successorOrder = m_cfg.reversePostOrderIndex(successor);
                isPending.quickSet(successorOrder);
                changed |= successorOrder <= blockOrder;
            }
        }
    }
}

UseEffectHistory::AtPoint::AtPoint(const UseEffectHistory& history, ProgramPoint point)
    : m_history(history)
    , m_isUnknown(history.m_gaveUp || !history.m_numberOfObjects || history.isSkipped(point.block))
    , m_index(point.index)
{
    RELEASE_ASSERT(point.index <= history.m_function.at(point.block).values.size());
    if (m_isUnknown)
        return;
    m_head = history.headOf(point.block);
    m_actions = history.actionsOf(point.block);
}

Occurrence UseEffectHistory::AtPoint::occurrenceOf(ObjectID object) const
{
    RELEASE_ASSERT(object.index() < m_history.m_numberOfObjects);
    if (m_isUnknown || m_history.m_isUnknown.quickGet(object.index()))
        return Occurrence::Unknown;

    bool hasOccurred = !!(m_head[wordIndexOf(object)] & maskOf(object));
    if (!m_actions.empty()) {
        // The last action in front of the point decides.
        auto actions = std::ranges::equal_range(m_actions, object, { }, &Action::object);
        auto end = std::ranges::lower_bound(actions, m_index, { }, &Action::index);
        if (end != actions.begin())
            hasOccurred = std::prev(end)->kind == ActionKind::Event;
    }
    return hasOccurred ? Occurrence::MayHaveOccurred : Occurrence::HasNotOccurred;
}

auto UseEffectHistory::actionsOf(BlockID block) const -> std::span<const Action>
{
    uint32_t blockOrder = m_cfg.reversePostOrderIndex(block);
    uint32_t first = m_firstAction[blockOrder];
    return m_actions.subspan(first, m_firstAction[blockOrder + 1] - first);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
