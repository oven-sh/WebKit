/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsWrittenOnceForFrontsOfLoops { AbstractHeap::ShapeWords };

void Walker::layFrontOfQuietTurns(OpenLoop& loop, BlockID inputHead)
{
    if (m_numberOfCopiesBuiltForSize || isUnreachable() || isUnderHandler() || hasTextsInStringBuilders() || m_code.at(m_block).handler)
        return;
    std::span<const ValueID> parameters = m_code.at(loop.head).parameters();
    if (loop.entering.size() != parameters.size())
        return;
    if (m_triesOfLoops.isEmpty() || m_triesOfLoops.last().frameIndex != m_frames.size() - 1 || m_triesOfLoops.last().inputHead != inputHead)
        return;
    loop.placeOfRow = m_triesOfLoops.last().answer + 1;
    if ((m_answersOfLoops[loop.placeOfRow - 1].whatTurnsShowed & (someWayRoundDidNothing | someReadCouldStandInFront)) != (someWayRoundDidNothing | someReadCouldStandInFront)) {
        loop.watchesTurns = true;
        return;
    }
    loop.placesThatQuietTurnsWrite = m_answersOfLoops[loop.placeOfRow - 1].placesThatQuietTurnsWrite;
    WayIntoClosedCopy* way = m_waysIntoClosedCopies.isEmpty() ? nullptr : &m_waysIntoClosedCopies.last();
    if (way && (way->frameIndex != m_frames.size() - 1 || way->inputHead != inputHead))
        way = nullptr;
    if (way) {
        if (!way->entry || !way->startFromInFront)
            return;
        std::span<const ValueID> taken = m_code.at(way->entry).parameters();
        if (taken.size() != parameters.size())
            return;
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (!m_types.isSubset(m_code.at(parameters[index]).type, m_code.at(taken[index]).type))
                return;
        }
    }

    BlockID front = m_code.addBlock(BlockRole::Ordinary, m_code.at(m_block).frequency);
    m_code.setLoopMarks(front, true, true);
    Vector<Reference, 8> heldAsReferences;
    for (ValueID parameter : parameters) {
        loop.heldByFront.append(m_code.appendParameter(front, m_code.at(parameter).type, m_code.at(parameter).representation, m_origin));
        heldAsReferences.append(Reference::value(loop.heldByFront.last()));
    }
    if (!loop.heldByFront.isEmpty())
        m_code.setParameters(front, loop.heldByFront.span());
    if (way) {
        loop.startOfClosedCopyFromFront = m_code.addBlock(BlockRole::Ordinary, BlockFrequency::Rare);
        m_code.setLoopMarks(loop.startOfClosedCopyFromFront, true, false);
        BlockID inFront = std::exchange(m_block, loop.startOfClosedCopyFromFront);
        endWithJumpInto(way->entry, heldAsReferences.span());
        m_block = inFront;
        if (m_hasFailed)
            return;
    }
    loop.frontOfQuietTurns = front;
    loop.blocksOfFrontOfQuietTurns.append(front);
}

void Walker::noteBlockOfFrontOfQuietTurns(uint32_t indexOfLoop, BlockID block)
{
    while (m_placesOfLoopsBehindBlocks.size() <= block.index())
        m_placesOfLoopsBehindBlocks.append(0);
    setPlaceOfLoopBehindBlock(block, indexOfLoop + 1);
}

bool Walker::belongsInFrontOfQuietTurns(uint32_t indexOfLoop, const Draft& draft, const Effects& effects) const
{
    const OpenLoop& loop = m_openLoops[indexOfLoop];
    if (!loop.frontOfQuietTurns)
        return false;
    LocationMasks reads = effects.reads.without(heapsWrittenOnceForFrontsOfLoops);
    if (!reads.isEmpty() && (loop.effects.mayRunOtherCode() || reads.overlaps(loop.effects.writes)))
        return true;
    return std::ranges::any_of(draft.operands(), [&](Reference operand) {
        return operand && operand.isValue() && loop.blocksOfFrontOfQuietTurns.contains(m_code.blockOf(operand.id()));
    });
}

BlockID Walker::rowInFrontOfLoopFor(uint32_t indexOfLoop, const Draft& draft, const Effects& effects)
{
    OpenLoop& loop = m_openLoops.changeable(indexOfLoop);
    if (!loop.frontOfQuietTurns || (m_block != loop.blockInFront && m_block != loop.blocksOfFrontOfQuietTurns.last()))
        return { };
    if (!belongsInFrontOfQuietTurns(indexOfLoop, draft, effects))
        return loop.blockInFront;
    loop.holdsReadsInFront = true;
    return loop.blocksOfFrontOfQuietTurns.last();
}

bool Walker::branchInFrontOfQuietTurns(uint32_t indexOfLoop, Reference test, bool passesWithYes)
{
    OpenLoop& loop = m_openLoops.changeable(indexOfLoop);
    if (!loop.frontOfQuietTurns || !loop.startOfClosedCopyFromFront || !loop.blocksOfFrontOfQuietTurns.contains(m_code.blockOf(test.id())))
        return false;
    BlockID front = loop.blocksOfFrontOfQuietTurns.last();
    if (m_code.at(front).hasEnded())
        return false;
    BlockID next = m_code.addBlock(BlockRole::Ordinary, m_code.at(front).frequency);
    m_code.setLoopMarks(next, true, false);
    TerminatorDraft terminator(TerminatorKind::Branch, loop.origin);
    terminator.setConditionOperand(test);
    m_code.end(front, terminator, 2);
    m_code.closeEdge(m_code.at(front).terminator.taken(), passesWithYes ? next : loop.startOfClosedCopyFromFront, { });
    m_code.closeEdge(m_code.at(front).terminator.notTaken(), passesWithYes ? loop.startOfClosedCopyFromFront : next, { });
    noteBlockOfFrontOfQuietTurns(indexOfLoop, next);
    loop.blocksOfFrontOfQuietTurns.append(next);
    return true;
}

void Walker::noteHeadOfQuietTurns(BlockID head)
{
    if (m_openLoops.isEmpty() || m_openLoops.last().head != head || (!m_openLoops.last().frontOfQuietTurns && !m_openLoops.last().watchesTurns) || isUnreachable())
        return;
    OpenLoop& loop = m_openLoops.changeableLast();
    const WayIntoClosedCopy* way = m_waysIntoClosedCopies.isEmpty() ? nullptr : &m_waysIntoClosedCopies.last();
    if (way && way->isEntered && way->frameIndex == loop.frame && inputBlock(way->frameIndex, way->inputHead).head == head && m_state.nothingWasDoneInTurn(way->numberOfTurn)) {
        loop.numberOfTurn = way->numberOfTurn;
        return;
    }
    loop.numberOfTurn = ++m_numberOfTypedCopiesEntered;
    m_state.noteHeadOfTurn(loop.numberOfTurn);
}

void Walker::noteWhatTurnsOfLoopShow(const OpenLoop& loop, uint8_t shown)
{
    m_answersOfLoops[loop.placeOfRow - 1].whatTurnsShowed |= shown;
}

void Walker::noteReadThatCouldStandInFront(const OpenLoop& loop, const LocationMasks& reads) const
{
    for (const TryOfLoop& tried : m_triesOfLoops) {
        if (tried.answer + 1 != loop.placeOfRow)
            continue;
        auto& seen = const_cast<TryOfLoop&>(tried).readsThatCouldStandInFront;
        if (!seen.contains(reads))
            seen.append(reads);
    }
}

bool Walker::quietTurnsWroteMoreThanRowSays(AnswerOfLoop& row, const TryOfLoop& tried)
{
    if (!(row.whatTurnsShowed & wasWalkedAgainForFront))
        return false;
    LocationMasks written = tried.writtenWithoutOtherCode.without(heapsWhoseWritesNobodySees);
    if (written.isSubsetOf(row.placesThatQuietTurnsWrite))
        return false;
    row.placesThatQuietTurnsWrite.merge(written);
    return true;
}

bool Walker::frontOfQuietTurnsIsWorthAWalk(AnswerOfLoop& row, const TryOfLoop& tried)
{
    if (row.whatTurnsShowed & wasWalkedAgainForFront)
        return false;
    LocationMasks written = tried.writtenWithoutOtherCode.without(heapsWhoseWritesNobodySees);
    bool someReadStaysAsItIsInQuietTurns = tried.readsThatCouldStandInFront.containsIf([&](const LocationMasks& reads) {
        return !reads.overlaps(written);
    });
    if (!(row.whatTurnsShowed & someWayRoundDidNothing) || !someReadStaysAsItIsInQuietTurns)
        return false;
    row.placesThatQuietTurnsWrite = written;
    row.whatTurnsShowed |= someReadCouldStandInFront | wasWalkedAgainForFront;
    return true;
}

bool Walker::mayReadInFrontOfQuietTurns(uint32_t indexOfLoop) const
{
    const OpenLoop& loop = m_openLoops[indexOfLoop];
    return (loop.frontOfQuietTurns || loop.watchesTurns) && !isUnreachable() && m_state.noOtherCodeRanInTurn(loop.numberOfTurn) && innermostOpenLoop() == indexOfLoop && !isUnderHandler() && !hasTextsInStringBuilders();
}

bool Walker::holdBackEdgeOfTurnThatDidSomething(EdgeID edge, BlockID head, std::span<const Reference> arguments)
{
    size_t place = m_openLoops.size();
    while (place && m_openLoops[place - 1].head != head)
        --place;
    if (!place--)
        return false;
    if (m_openLoops[place].watchesTurns)
        noteWhatTurnsOfLoopShow(m_openLoops[place], m_state.noOtherCodeRanInTurn(m_openLoops[place].numberOfTurn) ? someWayRoundDidNothing : 0);
    if (!m_openLoops[place].frontOfQuietTurns)
        return false;
    OpenLoop& loop = m_openLoops.changeable(place);
    if (m_state.noOtherCodeRanInTurn(loop.numberOfTurn))
        return false;

    BlockID landing = m_code.addBlock(BlockRole::Ordinary, edge || isUnreachable() ? BlockFrequency::Normal : m_code.at(m_block).frequency);
    m_code.setLoopMarks(landing, true, false);
    if (edge)
        m_code.closeEdge(edge, landing, { });
    else
        m_code.endWithJump(m_block, m_origin, landing, { });
    m_block = landing;
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    OpenLoop::EdgeHeldBack held;
    for (size_t index = 0; index < arguments.size(); ++index) {
        Representation form = m_code.at(parameters[index]).representation;
        Reference argument = isSameWord(representationOf(arguments[index]), form) ? arguments[index] : convert(arguments[index], form, m_origin);
        if (!argument || isUnreachable()) {
            fail("an edge hands on a value that nothing was built for"_s);
            return true;
        }
        held.arguments.append(argument.id());
    }
    m_code.end(m_block, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
    held.edge = m_code.at(m_block).terminator.next();
    m_openLoops.changeable(place).edgesHeldBack.append(WTF::move(held));
    m_block = { };
    return true;
}

BlockID Walker::closeFrontOfQuietTurns(const OpenLoop& loop)
{
    if (!loop.frontOfQuietTurns)
        return loop.head;
    for (const OpenLoop::EdgeHeldBack& held : loop.edgesHeldBack)
        m_code.closeEdge(held.edge, loop.holdsReadsInFront ? loop.frontOfQuietTurns : loop.head, held.arguments.span());
    BlockID last = loop.blocksOfFrontOfQuietTurns.last();
    if (!m_code.at(last).hasEnded()) {
        m_code.end(last, TerminatorDraft(TerminatorKind::Jump, loop.origin), 1);
        m_code.closeEdge(m_code.at(last).terminator.next(), loop.head, loop.heldByFront.span());
    }
    return loop.holdsReadsInFront ? loop.frontOfQuietTurns : loop.head;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
