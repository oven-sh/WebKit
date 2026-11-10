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
    if (m_coldCopyDepth || isUnreachable() || isUnderHandler() || hasStringsInBuilders() || m_code.at(m_block).handler)
        return;
    std::span<const ValueID> parameters = m_code.at(loop.head).parameters();
    if (loop.recordsEntering().size() != parameters.size())
        return;
    if (m_trialVisits.isEmpty() || m_trialVisits.last().frameIndex != m_frames.size() - 1 || m_trialVisits.last().inputHead != inputHead)
        return;
    loop.summaryIndexPlusOne = m_trialVisits.last().summaryIndex + 1;
    if ((m_loopSummaries[loop.summaryIndexPlusOne - 1].whatTurnsShowed & (someWayRoundDidNothing | someReadCouldStandInFront)) != (someWayRoundDidNothing | someReadCouldStandInFront)) {
        loop.watchesTurns = true;
        return;
    }
    loop.placesThatQuietTurnsWrite = m_loopSummaries[loop.summaryIndexPlusOne - 1].placesThatQuietTurnsWrite;
    VersionedLoop* versionedLoop = m_versionedLoops.isEmpty() ? nullptr : &m_versionedLoops.last();
    if (versionedLoop && (versionedLoop->frameIndex != m_frames.size() - 1 || versionedLoop->inputHead != inputHead))
        versionedLoop = nullptr;
    if (versionedLoop) {
        if (!versionedLoop->entry || !versionedLoop->genericStart)
            return;
        std::span<const ValueID> taken = m_code.at(versionedLoop->entry).parameters();
        if (numberOfFieldsOfScopesThatBlockTakes(versionedLoop->entry) || taken.size() + m_numberOfFieldsOfScopesOfHeadBeingOpened != parameters.size())
            return;
        for (size_t index = 0; index < taken.size(); ++index) {
            if (!m_types.isSubset(m_code.at(parameters[index]).type, m_code.at(taken[index]).type))
                return;
        }
    }

    BlockID front = m_code.addBlock(BlockRole::Ordinary, m_code.at(m_block).frequency);
    m_code.setLoopMarks(front, true, true);
    Vector<ValueID, 8> parametersOfFront;
    Vector<Reference, 8> heldAsReferences;
    for (ValueID parameter : parameters) {
        parametersOfFront.append(m_code.appendParameter(front, m_code.at(parameter).type, m_code.at(parameter).representation, m_origin));
        heldAsReferences.append(Reference::value(parametersOfFront.last()));
    }
    if (!parametersOfFront.isEmpty())
        m_code.setParameters(front, parametersOfFront.span());
    if (!makeReadyToCross(loop.head, heldAsReferences.span(), false, loop.heldByFront))
        return;
    if (versionedLoop) {
        loop.reloadFallbackBlock = m_code.addBlock(BlockRole::Ordinary, BlockFrequency::Rare);
        m_code.setLoopMarks(loop.reloadFallbackBlock, true, false);
        BlockID inFront = std::exchange(m_block, loop.reloadFallbackBlock);
        enterWithFieldsOfScopes(versionedLoop->entry, heldAsReferences.span().first(heldAsReferences.size() - m_numberOfFieldsOfScopesOfHeadBeingOpened));
        m_block = inFront;
        if (m_hasFailed)
            return;
    }
    loop.firstReloadBlock = front;
    loop.reloadBlocks.append(front);
}

void Walker::noteBlockOfFrontOfQuietTurns(uint32_t indexOfLoop, BlockID block)
{
    while (m_preheaderDepths.size() <= block.index())
        m_preheaderDepths.append(0);
    setPreheaderDepth(block, indexOfLoop + 1);
}

bool Walker::belongsInFrontOfQuietTurns(uint32_t indexOfLoop, const Draft& draft, const Effects& effects) const
{
    const OpenLoop& loop = m_openLoops[indexOfLoop];
    if (!loop.firstReloadBlock)
        return false;
    LocationMasks reads = effects.reads.without(heapsWrittenOnceForFrontsOfLoops);
    if (!reads.isEmpty() && (loop.effects.mayRunOtherCode() || reads.overlaps(loop.effects.writes)))
        return true;
    return std::ranges::any_of(draft.operands(), [&](Reference operand) {
        return operand && operand.isValue() && loop.reloadBlocks.contains(m_code.blockOf(operand.id()));
    });
}

BlockID Walker::rowInFrontOfLoopFor(uint32_t indexOfLoop, const Draft& draft, const Effects& effects)
{
    OpenLoop& loop = m_openLoops.changeable(indexOfLoop);
    if (!loop.firstReloadBlock || (m_block != loop.preheaderBlock && m_block != loop.reloadBlocks.last()))
        return { };
    if (!belongsInFrontOfQuietTurns(indexOfLoop, draft, effects))
        return loop.preheaderBlock;
    loop.holdsReadsInFront = true;
    return loop.reloadBlocks.last();
}

bool Walker::branchInFrontOfQuietTurns(uint32_t indexOfLoop, Reference test, bool passesWithYes)
{
    OpenLoop& loop = m_openLoops.changeable(indexOfLoop);
    if (!loop.firstReloadBlock || !loop.reloadFallbackBlock || !loop.reloadBlocks.contains(m_code.blockOf(test.id())))
        return false;
    BlockID front = loop.reloadBlocks.last();
    if (m_code.at(front).hasEnded())
        return false;
    BlockID next = m_code.addBlock(BlockRole::Ordinary, m_code.at(front).frequency);
    m_code.setLoopMarks(next, true, false);
    TerminatorDraft terminator(TerminatorKind::Branch, loop.origin);
    terminator.setConditionOperand(test);
    m_code.end(front, terminator, 2);
    m_code.closeEdge(m_code.at(front).terminator.taken(), passesWithYes ? next : loop.reloadFallbackBlock, { });
    m_code.closeEdge(m_code.at(front).terminator.notTaken(), passesWithYes ? loop.reloadFallbackBlock : next, { });
    noteBlockOfFrontOfQuietTurns(indexOfLoop, next);
    loop.reloadBlocks.append(next);
    return true;
}

void Walker::letFrontOfQuietTurnsFailUnasked(uint32_t indexOfLoop)
{
    if (!m_inputs.options.shouldFailSpeculations || !m_openLoops[indexOfLoop].reloadFallbackBlock) [[likely]]
        return;
    BlockID outside = std::exchange(m_block, m_openLoops[indexOfLoop].reloadBlocks.last());
    Reference fails = drawWhetherGuessFails(m_openLoops[indexOfLoop].origin);
    m_block = outside;
    if (fails && branchInFrontOfQuietTurns(indexOfLoop, fails, false))
        m_openLoops.changeable(indexOfLoop).holdsReadsInFront = true;
}

void Walker::noteHeadOfQuietTurns(BlockID head)
{
    if (m_openLoops.isEmpty() || m_openLoops.last().head != head || (!m_openLoops.last().firstReloadBlock && !m_openLoops.last().watchesTurns) || isUnreachable())
        return;
    OpenLoop& loop = m_openLoops.changeableLast();
    const VersionedLoop* versionedLoop = m_versionedLoops.isEmpty() ? nullptr : &m_versionedLoops.last();
    if (versionedLoop && versionedLoop->isEntered && versionedLoop->frameIndex == loop.frame && inputBlock(versionedLoop->frameIndex, versionedLoop->inputHead).head == head && m_state.isEffectFreeSince(versionedLoop->restartPoint)) {
        loop.restartPoint = versionedLoop->restartPoint;
        loop.effectsCheckBeginning = versionedLoop->effectsCheckBeginning;
        return;
    }
    loop.restartPoint = markRestartPoint();
    if (m_inputs.options.checksEffects) [[unlikely]] {
        Reference beginning = build(Draft(Opcode::BeginEffectsCheck, m_origin));
        m_openLoops.changeableLast().effectsCheckBeginning = beginning;
    }
}

void Walker::noteWhatTurnsOfLoopShow(const OpenLoop& loop, uint8_t shown)
{
    m_loopSummaries[loop.summaryIndexPlusOne - 1].whatTurnsShowed |= shown;
}

void Walker::noteReadThatCouldStandInFront(const OpenLoop& loop, const LocationMasks& reads) const
{
    for (const TrialVisit& trialVisit : m_trialVisits) {
        if (trialVisit.summaryIndex + 1 != loop.summaryIndexPlusOne)
            continue;
        auto& seen = const_cast<TrialVisit&>(trialVisit).readsThatCouldStandInFront;
        if (!seen.contains(reads))
            seen.append(reads);
    }
}

BitVector Walker::blocksThatSomeQuietTurnPassesThrough(BlockID inputHead, std::span<const ValueID> ranOtherCode)
{
    const BodyIndex& index = currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    BitVector runsOtherCode;
    for (ValueID ran : ranOtherCode)
        runsOtherCode.set(index.blockOf(ran).index());
    BitVector reachedFromHead;
    BitVector reachesBackEdge;
    if (runsOtherCode.get(inputHead.index()))
        return { };
    Vector<BlockID, 16> waiting;
    reachedFromHead.set(inputHead.index());
    waiting.append(inputHead);
    while (!waiting.isEmpty()) {
        BlockID block = waiting.takeLast();
        for (BodyTarget target : index.terminator(block).targets()) {
            BlockID next = target.block();
            if (next == inputHead && !reachesBackEdge.get(block.index())) {
                reachesBackEdge.set(block.index());
                continue;
            }
            if (!index.isInLoop(next, loop) || reachedFromHead.get(next.index()) || runsOtherCode.get(next.index()))
                continue;
            reachedFromHead.set(next.index());
            waiting.append(next);
        }
    }
    for (size_t latch : reachesBackEdge)
        waiting.append(BlockID(static_cast<uint32_t>(latch)));
    while (!waiting.isEmpty()) {
        BlockID block = waiting.takeLast();
        if (block == inputHead)
            continue;
        for (const BodyIncomingEdge& edge : incomingEdgesOf(index, block)) {
            if (!reachedFromHead.get(edge.from.index()) || reachesBackEdge.get(edge.from.index()))
                continue;
            reachesBackEdge.set(edge.from.index());
            waiting.append(edge.from);
        }
    }
    return reachesBackEdge;
}

void Walker::noteCallOnUsualWayInTries(const Effects& heard)
{
    if (!heard.mayRunOtherCode() || m_numberOfClosedArms || m_numberOfFormsHeardAsArms || m_code.at(m_block).frequency != BlockFrequency::Normal)
        return;
    for (TrialVisit& trialVisit : m_trialVisits) {
        if (!trialVisit.callOnUsualWayComesRound && m_frames.size() - 1 == trialVisit.frameIndex && trialVisit.frameIndex < m_operationsBeingWalkedInFrames.size())
            trialVisit.callOnUsualWayComesRound = wayBehindOperationComesRound(trialVisit.inputHead, m_operationsBeingWalkedInFrames[trialVisit.frameIndex]);
    }
}

bool Walker::wayBehindOperationComesRound(BlockID inputHead, ValueID operation) const
{
    const BodyIndex& index = currentBodyIndex();
    if (!operation || operation.index() >= index.numberOfValues())
        return false;
    LoopID loop = index.loopOf(inputHead);
    BlockID from = index.blockOf(operation);
    if (!index.isInLoop(from, loop))
        return false;
    BitVector seen;
    Vector<BlockID, 16> waiting;
    waiting.append(from);
    seen.set(from.index());
    while (!waiting.isEmpty()) {
        BlockID block = waiting.takeLast();
        for (BodyTarget target : index.terminator(block).targets()) {
            BlockID next = target.block();
            if (next == inputHead)
                return true;
            if (!index.isInLoop(next, loop) || seen.get(next.index()))
                continue;
            seen.set(next.index());
            waiting.append(next);
        }
    }
    return false;
}

bool Walker::someTextGrowsInQuietTurn(BlockID inputHead, std::span<const GrowingText> texts, const TrialVisit& trialVisit)
{
    const BodyIndex& index = currentBodyIndex();
    Vector<ValueID, 4> ranOtherCode;
    for (ValueID ran : trialVisit.operationsThatRanOtherCode) {
        if (!ran || ran.index() >= index.numberOfValues() || index.isParameter(ran))
            return true;
        if (!opcodesOfFamily(OperationFamily::Operator).contains(index.opcodeOf(ran)))
            ranOtherCode.append(ran);
    }
    BitVector passedInQuietTurns = blocksThatSomeQuietTurnPassesThrough(inputHead, ranOtherCode.span());
    if (!passedInQuietTurns.get(inputHead.index()))
        return true;
    for (const GrowingText& text : texts) {
        for (const NameOfGrowingText& name : text.names) {
            if (!index.isParameter(name.value) && passedInQuietTurns.get(index.blockOf(name.value).index()))
                return true;
        }
        if (!text.parametersOfInnerHeads.isEmpty())
            return true;
    }
    return false;
}

static uint64_t keyOfInputLoopForFrontsOfLoops(FunctionID function, BlockID inputHead)
{
    return static_cast<uint64_t>(function.index()) << 32 | inputHead.index();
}

void Walker::keepTextsOfLoopOutOfBuilders(BlockID inputHead)
{
    m_loopsWhoseTextsStayOutOfBuilders.add(keyOfInputLoopForFrontsOfLoops(currentFunction(), inputHead));
}

bool Walker::textsOfLoopStayOutOfBuilders(BlockID inputHead) const
{
    return !m_loopsWhoseTextsStayOutOfBuilders.isEmpty() && m_loopsWhoseTextsStayOutOfBuilders.contains(keyOfInputLoopForFrontsOfLoops(currentFunction(), inputHead));
}

bool Walker::quietTurnsWroteMoreThanRowSays(LoopSummary& summary, const TrialVisit& trialVisit)
{
    if (!(summary.whatTurnsShowed & wasWalkedAgainForFront))
        return false;
    LocationMasks written = trialVisit.writtenWithoutOtherCode.without(unobservableWriteHeaps);
    if (written.isSubsetOf(summary.placesThatQuietTurnsWrite))
        return false;
    summary.placesThatQuietTurnsWrite.merge(written);
    return true;
}

bool Walker::frontOfQuietTurnsIsWorthAWalk(LoopSummary& summary, const TrialVisit& trialVisit)
{
    if (summary.whatTurnsShowed & wasWalkedAgainForFront)
        return false;
    LocationMasks written = trialVisit.writtenWithoutOtherCode.without(unobservableWriteHeaps);
    bool someReadStaysAsItIsInQuietTurns = trialVisit.readsThatCouldStandInFront.containsIf([&](const LocationMasks& reads) {
        return !reads.overlaps(written);
    });
    if (!(summary.whatTurnsShowed & someWayRoundDidNothing) || !someReadStaysAsItIsInQuietTurns)
        return false;
    summary.placesThatQuietTurnsWrite = written;
    summary.whatTurnsShowed |= someReadCouldStandInFront | wasWalkedAgainForFront;
    return true;
}

bool Walker::mayReadInFrontOfQuietTurns(uint32_t indexOfLoop) const
{
    const OpenLoop& loop = m_openLoops[indexOfLoop];
    return (loop.firstReloadBlock || loop.watchesTurns) && !isUnreachable() && m_state.hasOnlyKnownEffectsSince(loop.restartPoint) && innermostOpenLoop() == indexOfLoop && !isUnderHandler() && !hasStringsInBuilders();
}

bool Walker::holdBackEdgeOfTurnThatDidSomething(EdgeID& edge, BlockID head, std::span<const Reference> arguments)
{
    size_t place = m_openLoops.size();
    while (place && m_openLoops[place - 1].head != head)
        --place;
    if (!place--)
        return false;
    if (m_openLoops[place].watchesTurns)
        noteWhatTurnsOfLoopShow(m_openLoops[place], m_state.hasOnlyKnownEffectsSince(m_openLoops[place].restartPoint) ? someWayRoundDidNothing : 0);
    if (!m_openLoops[place].firstReloadBlock)
        return false;
    OpenLoop& loop = m_openLoops.changeable(place);
    if (m_state.hasOnlyKnownEffectsSince(loop.restartPoint)) {
        if (Reference beginning = loop.effectsCheckBeginning; beginning && beginning.isValue()) [[unlikely]] {
            if (edge)
                layBlockOnEdge(edge, head);
            Draft quiet(Opcode::CheckReloadBypass, m_origin);
            quiet.setBeginningOperand(beginning);
            build(WTF::move(quiet));
        }
        return false;
    }

    BlockID landing = m_code.addBlock(BlockRole::Ordinary, edge || isUnreachable() ? BlockFrequency::Normal : m_code.at(m_block).frequency);
    m_code.setLoopMarks(landing, true, false);
    if (edge)
        m_code.closeEdge(edge, landing, { });
    else
        m_code.endWithJump(m_block, m_origin, landing, { });
    m_block = landing;
    OpenLoop::EdgeHeldBack held;
    if (!makeReadyToCross(head, arguments, true, held.arguments))
        return true;
    m_code.end(m_block, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
    held.edge = m_code.at(m_block).terminator.next();
    m_openLoops.changeable(place).edgesHeldBack.append(WTF::move(held));
    m_block = { };
    return true;
}

BlockID Walker::closeFrontOfQuietTurns(const OpenLoop& loop)
{
    if (!loop.firstReloadBlock)
        return loop.head;
    if (loop.holdsReadsInFront) {
        count(StatisticID::LoopReloadBlocksEmitted);
        count(StatisticID::ReloadingBackEdgesEmitted, loop.edgesHeldBack.size());
    }
    for (const OpenLoop::EdgeHeldBack& held : loop.edgesHeldBack)
        m_code.closeEdge(held.edge, loop.holdsReadsInFront ? loop.firstReloadBlock : loop.head, held.arguments);
    BlockID last = loop.reloadBlocks.last();
    if (!m_code.at(last).hasEnded()) {
        m_code.end(last, TerminatorDraft(TerminatorKind::Jump, loop.origin), 1);
        m_code.closeEdge(m_code.at(last).terminator.next(), loop.head, loop.heldByFront);
    }
    return loop.holdsReadsInFront ? loop.firstReloadBlock : loop.head;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
