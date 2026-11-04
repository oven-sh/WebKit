/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A poll throws the out-of-memory error that an operation has left owed because it could not throw it. Never withheld: under a handler a poll stands at once behind a store that makes room without collecting.
// Handlers: the poll of a loop under a handler from outside the loop leaves that handler where no likely turn may collect. Folding: the poll of a loop that never turns goes.
// At a suspension: nothing is owed there, and a loop that may suspend keeps its poll as it is. At a throw: a poll that has left its handler leaves the error owed (operationAOTPollSlow).

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTOpcodeSets.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// `o.b = 2` where the cell has no room left. The store lays new storage into its owner, and where there is no memory for that it can neither collect nor throw: whichever poll comes next throws for it.
static bool storeMayLeaveErrorOwedForPolls(Opcode opcode)
{
    // On a receiver that nobody knows the stub ends in the same operation.
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

// What stands behind a poll gets a handler edge of its own, so the edge of `from` is there for the poll alone.
static BlockID jumpIntoNewBlockForPolls(Code& code, BlockID from, Origin origin)
{
    BlockID onward = code.addBlock(BlockRole::Ordinary, code.at(from).frequency);
    code.setLoopMarks(onward, code.at(from).isInLoop, false);
    code.end(from, TerminatorDraft(TerminatorKind::Jump, origin), 1);
    code.closeEdge(code.at(from).terminator.next(), onward, { });
    return onward;
}

// One live poll for a run. `cause` was appended just now, and a poll is about to be built behind it, in its block and so under the same handler edge.
// A poll that throws for a store goes only where nobody sees anything happen between it and the later one. A poll in front of a loop ends its block, so none is met here.
static void dropPollThatLaterOneStandsForForPolls(Code& code, ValueID cause, bool causeMayRunOtherCode)
{
    bool standsOnRareWay = code.at(code.blockOf(cause)).frequency == BlockFrequency::Rare;
    bool hasPassedWhatIsSeen = false;
    for (ValueID id = code.at(cause).previous; id; id = code.at(id).previous) {
        const Operation& record = code.at(id);
        if (record.opcode != Opcode::LoopHint) {
            if (record.traits().isUnobservable())
                continue;
            // Only a rare way round a loop holds polls that throw for no store.
            if (!standsOnRareWay)
                return;
            hasPassedWhatIsSeen = true;
            continue;
        }
        if (!record.throwsOwedError())
            return;

        // Between a poll and what it throws for stands what its handler edge brings.
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
        // The hint that the body has in the head of a loop.
        const Frame& frame = m_frames.last();
        if (draft.flags || !draft.inputValueGiven || frame.placesOfOpenLoops.isEmpty() || !frame.index->isLoopHead(frame.block))
            return;
        uint32_t place = frame.placesOfOpenLoops[frame.index->loopOf(frame.block).index()];
        if (!place || m_openLoops[place - 1].poll)
            return;
        m_openLoops[place - 1].poll = id;
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

    // Nothing that may throw is built while a handler edge is led, or in front of a loop from inside it.
    bool mayBuildPoll = !m_isLeadingHandlerEdge && m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() && isUnderHandler();

    // Something may come to be owed in a turn of the loops around. On a rare way round a poll behind it throws that, and the likely turns go on without.
    bool pollsOnRareWay = false;
    if (speaksOfLoopsAround) {
        bool standsOnRareWay = mayBuildPoll && !m_numberOfCopiesBuiltForSize && m_code.at(m_block).frequency == BlockFrequency::Rare && familyOf(draft.opcode) != OperationFamily::Exception;
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
                OpenLoop& open = m_openLoops[place - 1];
                // It has gone on to every loop around.
                if (open.likelyTurnMayCollect)
                    isSettled = true;
                else if (!standsOnRareWay)
                    open.likelyTurnMayCollect = true;
                else if (open.pollInFront) {
                    pollsOnRareWay = true;
                    isSettled = true;
                }
            }
        }
    }

    // Outside every handler the error waits for the next poll, of the function or of its caller.
    if (!mayBuildPoll || (!isStoreThatMayOwe && !pollsOnRareWay))
        return;
    // A setter that a later store of the run enters finds the error at its own prologue.
    dropPollThatLaterOneStandsForForPolls(m_code, id, effects.mayRunOtherCode() && !isStoreThatMayOwe);

    // The error belongs to what stands in front of the poll. A poll is no call: it stands in no tail position and constructs nothing.
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
    // Only a loop that never suspends is open while it is walked. A copy that is built for size keeps its poll as it is.
    if (!has(Mechanism::Handlers) || preview.effects.maySuspend || m_numberOfCopiesBuiltForSize || isUnreachable() || !isUnderHandler())
        return;

    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    BodyBlock head = index.block(inputHead);
    // An edge from here to a handler block of the loop would enter the loop past its head.
    if (head.hasHandler() && index.isInLoop(head.handler().block(), index.loopOf(inputHead)))
        return;

    // The poll stands under the handler of the head, with what enters in the place of the head's parameters. noteStateAtThrowForHandler() takes a handler that no head has noted for one of which nothing is known.
    if (head.hasHandler())
        noteStateAtHeadForHandler();
    Draft poll(Opcode::LoopHint, m_origin);
    poll.setThrowsOwedError(true);
    Reference built = build(WTF::move(poll));
    if (!built || isUnreachable())
        return;

    // What leaves the loop is appended to the block in front of it, which takes nothing that may collect while it has a handler edge: findLoopToBuildInFrontOf().
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
            ++m_openLoops[place - 1].numberOfEdgesThatCameRound;
            return;
        }
    }
}

void Walker::settleFateOfPoll(const OpenLoop& loop)
{
    // A failure may have taken the frame away.
    if (m_hasFailed || loop.frame >= m_frames.size())
        return;

    // The innermost open loop around this one: of its own body, or around the call in whose place its body is walked.
    OpenLoop* around = nullptr;
    for (size_t frameIndex = loop.frame + 1; frameIndex-- && !around;) {
        const Frame& frame = m_frames[frameIndex];
        if (frame.placesOfOpenLoops.isEmpty())
            continue;
        LoopID first = frameIndex == loop.frame ? frame.index->loop(loop.loop).parent : frame.index->loopOf(frame.block);
        for (LoopID candidate = first; candidate && !around; candidate = frame.index->loop(candidate).parent) {
            if (uint32_t place = frame.placesOfOpenLoops[candidate.index()])
                around = &m_openLoops[place - 1];
        }
    }
    auto drop = [&](ValueID poll) {
        if (poll)
            m_code.addFateOfPoll(poll, FateOfPoll::IsDropped);
    };

    if (around)
        around->likelyTurnMayCollect |= loop.likelyTurnMayCollect;

    // `do { .. } while (false)`, `for (..) { ..; break }`: what was written as a loop goes straight on.
    if (!loop.numberOfEdgesThatCameRound && has(Mechanism::Folding)) {
        drop(loop.poll);
        drop(loop.pollInFront);
        if (around)
            around->pollsInFrontOfLoopsInside.appendVector(loop.pollsInFrontOfLoopsInside);
        return;
    }

    bool pollLeavesHandler = loop.poll && m_code.at(m_code.blockOf(loop.poll)).handler && loop.pollInFront && !loop.likelyTurnMayCollect && loop.numberOfEdgesThatCameRound;
    if (!pollLeavesHandler) {
        drop(loop.pollInFront);
        return;
    }
    m_code.addFateOfPoll(loop.poll, FateOfPoll::LeavesHandler);
    // Nothing comes to be owed in a turn, so nothing is owed in front of a loop inside.
    for (ValueID poll : loop.pollsInFrontOfLoopsInside)
        drop(poll);
    if (around)
        around->pollsInFrontOfLoopsInside.append(loop.pollInFront);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
