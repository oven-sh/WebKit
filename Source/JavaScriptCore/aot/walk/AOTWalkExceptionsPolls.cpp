/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTOpcodeSets.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static ALWAYS_INLINE bool storeMayLeaveErrorOwedForPolls(Opcode opcode)
{
    if (opcode == Opcode::PropSet)
        return true;
    if (!runsOwnerBarrierOf(opcode))
        return false;
    const Effects& bound = declaredBoundInTable(opcode);
    return bound.writes.contains(AbstractHeap::AllocatorState) && !bound.mayCollect && !bound.mayThrow;
}
static_assert(runsOwnerBarrierOf(Opcode::PutSlotOrAdd) && declaredBoundOf(Opcode::PutSlotOrAdd).writes.contains(AbstractHeap::AllocatorState));
static_assert(!declaredBoundOf(Opcode::PutSlotOrAdd).mayCollect && !declaredBoundOf(Opcode::PutSlotOrAdd).mayThrow);
static_assert(resultRuleOf(Opcode::LoopHint) == ResultRule::None && mayCollect(Opcode::LoopHint) && mayThrow(Opcode::LoopHint));

static BlockID jumpIntoNewBlockForPolls(Code& code, BlockID from, Origin origin)
{
    BlockID onward = code.addBlock(BlockRole::Ordinary, code.at(from).frequency);
    code.setLoopMarks(onward, code.at(from).isInLoop, false);
    code.end(from, TerminatorDraft(TerminatorKind::Jump, origin), 1);
    code.closeEdge(code.at(from).terminator.next(), onward, { });
    return onward;
}

static void dropPollThatLaterOneStandsForForPolls(Code& code, ValueID cause, bool causeMayRunOtherCode)
{
    bool standsOnRareWay = code.at(code.blockOf(cause)).frequency == BlockFrequency::Rare;
    bool hasPassedWhatIsSeen = false;
    for (ValueID id = code.at(cause).previous; id; id = code.at(id).previous) {
        const Operation& record = code.at(id);
        if (record.opcode != Opcode::LoopHint) {
            if (record.traits().isUnobservable())
                continue;
            if (!standsOnRareWay)
                return;
            hasPassedWhatIsSeen = true;
            continue;
        }
        if (!record.throwsOwedError())
            return;

        ValueID polledFor = record.previous;
        while (polledFor && conversionsOfForm.contains(code.at(polledFor).opcode))
            polledFor = code.at(polledFor).previous;
        bool throwsForStore = !polledFor || storeMayLeaveErrorOwedForPolls(code.at(polledFor).opcode);
        if (!throwsForStore || (!hasPassedWhatIsSeen && !causeMayRunOtherCode))
            code.addFateOfPoll(id, FateOfPoll::IsDropped);
        return;
    }
}

void Walker::noteRecordBuiltForPolls(ValueID id, const Draft& draft, const Effects& effects)
{
    if (draft.opcode == Opcode::LoopHint) {
        const Frame& frame = m_frames.last();
        if (draft.flags || !draft.inputValueGiven || frame.placesOfOpenLoops.isEmpty() || !frame.index->isLoopHead(frame.block))
            return;
        uint32_t place = frame.placesOfOpenLoops[frame.index->loopOf(frame.block).index()];
        if (!place || m_openLoops[place - 1].poll)
            return;
        m_openLoops.changeable(place - 1).poll = id;
        if (m_openLoops[place - 1].pollInFront && !isUnreachable() && m_code.at(m_block).handler)
            m_block = jumpIntoNewBlockForPolls(m_code, m_block, m_origin);
        return;
    }

    bool isStoreThatMayOwe = storeMayLeaveErrorOwedForPolls(draft.opcode);
    bool speaksOfLoopsAround = (isStoreThatMayOwe || effects.mayCollect) && has(Mechanism::Handlers) && !m_openLoops.isEmpty();
    if (!isStoreThatMayOwe && !speaksOfLoopsAround)
        return;
    if (isUnreachable() || isNeverType(m_code.at(id).type))
        return;

    bool mayBuildPoll = !m_isLeadingHandlerEdge && m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() && isUnderHandler();

    bool pollsOnRareWay = false;
    if (speaksOfLoopsAround) {
        bool standsOnRareWay = mayBuildPoll && !m_coldCopyDepth && m_code.at(m_block).frequency == BlockFrequency::Rare && familyOf(draft.opcode) != OperationFamily::Exception;
        uint32_t numberOfLoops = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
        bool isSettled = false;
        for (size_t frameIndex = m_frames.size(); frameIndex-- && !isSettled;) {
            const Frame& frame = m_frames[frameIndex];
            if (frame.placesOfOpenLoops.isEmpty())
                continue;
            for (LoopID loop = frame.index->loopOf(frame.block); loop && !isSettled; loop = frame.index->loop(loop).parent) {
                uint32_t place = frame.placesOfOpenLoops[loop.index()];
                if (!place || place > numberOfLoops)
                    continue;
                OpenLoop& open = m_openLoops.changeable(place - 1);
                if (open.likelyTurnMayCollect)
                    isSettled = true;
                else if (!standsOnRareWay && m_makesLetterOfStep && !isStoreThatMayOwe)
                    open.lettersMadeInLikelyTurns.append(id);
                else if (!standsOnRareWay)
                    open.likelyTurnMayCollect = true;
                else if (open.pollInFront) {
                    pollsOnRareWay = true;
                    isSettled = true;
                }
            }
        }
    }

    if (!mayBuildPoll || (!isStoreThatMayOwe && !pollsOnRareWay))
        return;
    dropPollThatLaterOneStandsForForPolls(m_code, id, effects.mayRunOtherCode() && !isStoreThatMayOwe);

    Origin origin = draft.origin();
    origin.isTailCall = false;
    origin.isPartOfConstruction = false;
    Draft poll(Opcode::LoopHint, origin);
    poll.setThrowsOwedError(true);
    build(WTF::move(poll));
}

void Walker::buildPollInFrontOfLoop(BlockID inputHead, const LoopPreview& preview)
{
    m_pollInFrontOfLoopBeingEntered = { };
    if (!has(Mechanism::Handlers) || preview.effects.maySuspend || m_coldCopyDepth || isUnreachable() || !isUnderHandler())
        return;

    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    BodyBlock head = index.block(inputHead);
    if (head.hasHandler() && index.isInLoop(head.handler().block(), index.loopOf(inputHead)))
        return;

    if (head.hasHandler())
        noteStateAtHeadForHandler();
    Draft poll(Opcode::LoopHint, m_origin);
    poll.setThrowsOwedError(true);
    Reference built = build(WTF::move(poll));
    if (!built || isUnreachable())
        return;

    m_pollInFrontOfLoopBeingEntered = built.id();
    m_block = jumpIntoNewBlockForPolls(m_code, m_block, m_origin);
}

void Walker::noteEdgeCameRound(BlockID head)
{
    const Frame& frame = m_frames.last();
    if (frame.placesOfOpenLoops.isEmpty())
        return;
    for (LoopID loop = frame.index->loopOf(frame.block); loop; loop = frame.index->loop(loop).parent) {
        uint32_t place = frame.placesOfOpenLoops[loop.index()];
        if (place && m_openLoops[place - 1].head == head) {
            ++m_openLoops.changeable(place - 1).numberOfEdgesThatCameRound;
            return;
        }
    }
}

void Walker::settleFateOfPoll(const OpenLoop& loop)
{
    if (m_hasFailed || loop.frame >= m_frames.size())
        return;

    OpenLoop* around = nullptr;
    for (size_t frameIndex = loop.frame + 1; frameIndex-- && !around;) {
        const Frame& frame = m_frames[frameIndex];
        if (frame.placesOfOpenLoops.isEmpty())
            continue;
        LoopID first = frameIndex == loop.frame ? frame.index->loop(loop.loop).parent : frame.index->loopOf(frame.block);
        for (LoopID candidate = first; candidate && !around; candidate = frame.index->loop(candidate).parent) {
            if (uint32_t place = frame.placesOfOpenLoops[candidate.index()])
                around = &m_openLoops.changeable(place - 1);
        }
    }
    auto drop = [&](ValueID poll) {
        if (poll)
            m_code.addFateOfPoll(poll, FateOfPoll::IsDropped);
    };

    if (around)
        around->likelyTurnMayCollect |= loop.likelyTurnMayCollect;

    if (!loop.numberOfEdgesThatCameRound && has(Mechanism::Folding)) {
        drop(loop.poll);
        drop(loop.pollInFront);
        if (around)
            around->pollsInFrontOfLoopsInside.appendVector(loop.pollsInFrontOfLoopsInside);
        return;
    }

    bool pollLeavesHandler = loop.poll && m_code.at(m_code.blockOf(loop.poll)).handler && loop.pollInFront && !loop.likelyTurnMayCollect && loop.numberOfEdgesThatCameRound;
    if (!pollLeavesHandler || (!loop.lettersMadeInLikelyTurns.isEmpty() && !loop.pollsInFrontOfLoopsInside.isEmpty())) {
        drop(loop.pollInFront);
        return;
    }
    m_code.addFateOfPoll(loop.poll, FateOfPoll::LeavesHandler);
    for (ValueID letter : loop.lettersMadeInLikelyTurns)
        m_code.addFateOfPoll(loop.poll, FateOfPoll::LeavesHandler, letter);
    for (ValueID poll : loop.pollsInFrontOfLoopsInside)
        drop(poll);
    if (around && loop.lettersMadeInLikelyTurns.isEmpty())
        around->pollsInFrontOfLoopsInside.append(loop.pollInFront);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
