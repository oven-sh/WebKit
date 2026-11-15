/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"
#include "AOTWalkCountersOfLoops.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkStrings.h"

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
    if (draft.opcode == Opcode::FlattenString)
        reads = { };
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
    if (versionedLoop && isIterationEffectFree()) {
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

bool Walker::isCallQuietForArguments(FunctionID callee) const
{
    const LinkedEffectsGivenArguments* given = facts().linked().known.findEffectsGivenArguments(callee);
    return given && !given->parametersCalled && !given->onReturn.mayRunOtherCode() && !given->onThrow.mayRunOtherCode();
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

static bool stepsThroughList(Walker& walker, const BodyIndex& index, BlockID inputHead)
{
    return std::ranges::any_of(index.block(inputHead).values(), [&](const BodyValue& value) {
        return findIndexedArrayIteration(walker, index, value, true);
    });
}

bool Walker::isRecordUnswitchable(uint32_t openLoop, ValueID record, unsigned depth, LocationMasks& reads) const
{
    if (m_code.isConstant(record) || isInvariantInLoop(openLoop, Reference::value(record)))
        return true;
    const Operation& made = m_code.at(record);
    if (!depth || made.opcode == Opcode::Parameter)
        return false;
    Effects effects = declaredBoundOf(made.opcode);
    if (effects.mayRunOtherCode() || !effects.mayBeRunAgainUnseen())
        return false;
    if (made.opcode == Opcode::GetModuleVariable)
        reads.add(program(), MemoryLocation::moduleVariable(made.variable()));
    else
        reads.merge(effects.reads);
    bool isUnswitchable = true;
    made.forEachOperand([&](ValueID operand, uint32_t) {
        isUnswitchable = isUnswitchable && (!operand || isRecordUnswitchable(openLoop, operand, depth - 1, reads));
    });
    return isUnswitchable;
}

bool Walker::isConditionUnswitchable(uint32_t openLoop, ValueID written, unsigned depth, LocationMasks& reads) const
{
    const BodyIndex& index = currentBodyIndex();
    if (!written)
        return false;
    Reference bound = referenceBoundTo(written);
    if (bound && (bound.isConstant() || (bound.isValue() && (m_code.isConstant(bound.id()) || isInvariantInLoop(openLoop, bound)))))
        return true;
    ValueID value = index.stripCasts(written);
    if (!value || !depth)
        return false;
    if (index.isParameter(value))
        return bound && bound.isValue() && isRecordUnswitchable(openLoop, bound.id(), depth, reads);
    BodyValue operation = index.value(value);
    Opcode opcode = operation.opcode();
    static constexpr OpcodeSet operators = allRelations | allEqualities | OpcodeSet { Opcode::ToBoolean, Opcode::LogicalNot, Opcode::CheckInitialized, Opcode::TypeTest };
    if (primitiveConstants.contains(opcode) || opcode == Opcode::Builtin || opcode == Opcode::String)
        return true;
    if (opcode == Opcode::Call) {
        ValueID callee = index.stripCasts(operation.calleeOperand());
        bool isLookup = callee && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::PropGet && operation.thisValueOperand() && operation.argumentsOperands().size() == 1;
        return isLookup && isConditionUnswitchable(openLoop, operation.thisValueOperand(), depth - 1, reads) && isConditionUnswitchable(openLoop, operation.argumentsOperands()[0], depth - 1, reads);
    }
    if (opcode == Opcode::PropGet || opcode == Opcode::PrivateGet)
        reads.add(program(), MemoryLocation::objectField(keyOfAccessToSlot(program(), operation)));
    else if (opcode == Opcode::ElemGet)
        reads.merge(readsOfOwnKeyLookup(program()));
    else if (opcode == Opcode::GetModuleVariable || opcode == Opcode::GetEnvironmentField)
        reads.merge(declaredEffectsOfOperation(program(), opcode, operation).reads);
    else if (!operators.contains(opcode))
        return false;
    bool isUnswitchable = true;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        isUnswitchable = isUnswitchable && (!operand || isConditionUnswitchable(openLoop, operand, depth - 1, reads));
    });
    return isUnswitchable;
}

static bool callsInDominatedBlocks(const BodyIndex& index, BlockID block)
{
    static constexpr OpcodeSet calls { Opcode::Call, Opcode::CallList, Opcode::Construct };
    for (uint32_t number = index.dominatorNumber(block); number <= index.lastDominatorNumberBelow(block); ++number) {
        for (BodyValue operation : index.block(index.blocksInDominatorOrder()[number]).values()) {
            if (calls.contains(operation.opcode()))
                return true;
        }
    }
    return false;
}

static std::optional<uint32_t> quietTargetOf(const BodyIndex& index, const BodyTerminator& branch)
{
    auto callsOnlyBehind = [&](uint32_t target) {
        BlockID block = branch.targets()[target].block();
        return index.numberOfIncomingEdges(block) == 1 && !index.isEnteredByHandlerEdge(block) && callsInDominatedBlocks(index, block);
    };
    bool callsWhereTaken = callsOnlyBehind(0);
    if (callsWhereTaken == callsOnlyBehind(1))
        return std::nullopt;
    return callsWhereTaken ? 1 : 0;
}

void Walker::noteUnswitchCandidate(const BodyTerminator& branch)
{
    UnswitchCollector& collector = m_unswitchCollectors.last();
    if (m_trialVisits.isEmpty() || m_trialVisits.last().frameIndex != collector.frameIndex || m_trialVisits.last().inputHead != collector.inputHead)
        return;
    if (!collector.collectsBranches)
        return;
    const BodyIndex& index = currentBodyIndex();
    bool isInFrameOfLoop = m_frames.size() - 1 == collector.frameIndex;
    LoopID loop = m_frames[collector.frameIndex].index->loopOf(collector.inputHead);
    if (isInFrameOfLoop && (branch.block() == collector.inputHead || !index.isInLoop(branch.block(), loop)))
        return;
    LoopID inner = index.loopOf(branch.block());
    if (inner && !(isInFrameOfLoop && inner == loop) && std::ranges::any_of(branch.targets(), [&](const BodyTarget& target) { return !index.isInLoop(target.block(), inner); }))
        return;
    FunctionID function = m_frames.last().function;
    std::optional<uint32_t> openLoop;
    for (uint32_t place = m_openLoops.size(); place-- && !openLoop;) {
        if (m_openLoops[place].frame == collector.frameIndex && m_openLoops[place].loop == loop)
            openLoop = place;
    }
    if (!openLoop)
        return;
    ValueID condition = branch.as<TerminatorKind::Branch>().conditionOperand();
    size_t known = collector.candidates.findIf([&](const UnswitchCandidate& candidate) { return candidate.function == function && candidate.block == branch.block(); });
    if (known == notFound) {
        LocationMasks reads;
        if (!isConditionUnswitchable(*openLoop, condition, 8, reads))
            return;
        std::optional<uint32_t> quietTarget = quietTargetOf(index, branch);
        if (!quietTarget || (isInFrameOfLoop && !index.isInLoop(branch.targets()[*quietTarget].block(), loop)))
            return;
        known = collector.candidates.size();
        collector.candidates.append(UnswitchCandidate { function, branch.block(), *quietTarget, reads });
    }
    bool isVersion = !m_versionedLoops.isEmpty() && m_versionedLoops.last().frameIndex == collector.frameIndex && m_versionedLoops.last().inputHead == collector.inputHead;
    if (!isVersion)
        return;
    bool isLoud = m_openLoops[*openLoop].effects.mayRunOtherCode();
    bool isHoistable = canHoistUnswitchCondition(*openLoop, condition, 6, isLoud ? UnswitchedIterations::AreQuiet : UnswitchedIterations::AsWalked);
    if (!isLoud || isHoistable)
        collector.candidates[known].isHoistable = isHoistable;
}

bool Walker::isQuietVersionOfUnswitchLoop() const
{
    if (m_unswitchCollectors.isEmpty() || isInUnswitchedVersion() || m_trialVisits.isEmpty() || m_versionedLoops.isEmpty())
        return false;
    const UnswitchCollector& collector = m_unswitchCollectors.last();
    auto isLoopOfCollector = [&](const auto& other) { return other.frameIndex == collector.frameIndex && other.inputHead == collector.inputHead; };
    if (!isLoopOfCollector(m_trialVisits.last()) || !isLoopOfCollector(m_versionedLoops.last()))
        return false;
    std::optional<uint32_t> openLoop = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
    return openLoop && !m_openLoops[*openLoop].effects.mayRunOtherCode();
}

bool Walker::isQuietIterationOfUnswitchLoop() const
{
    if (m_unswitchCollectors.isEmpty() || !m_unswitchCollectors.last().collectsInt32s || isInUnswitchedVersion() || m_trialVisits.isEmpty())
        return false;
    const UnswitchCollector& collector = m_unswitchCollectors.last();
    if (m_trialVisits.last().frameIndex != collector.frameIndex || m_trialVisits.last().inputHead != collector.inputHead)
        return false;
    LoopID loop = m_frames[collector.frameIndex].index->loopOf(collector.inputHead);
    for (size_t place = m_openLoops.size(); place--;) {
        if (m_openLoops[place].frame == collector.frameIndex && m_openLoops[place].loop == loop)
            return !m_openLoops[place].effects.mayRunOtherCode();
    }
    return false;
}

void Walker::noteInt32Candidate()
{
    auto& candidates = m_unswitchCollectors.last().candidates;
    if (candidates.containsIf([](const UnswitchCandidate& candidate) { return candidate.asksForInt32s; }))
        return;
    UnswitchCandidate candidate;
    candidate.asksForInt32s = true;
    candidates.append(candidate);
}

static constexpr size_t fewestReadsOffVersionedList = 3;

void Walker::noteNumericRead(ValueID read, Reference list)
{
    UnswitchCollector& collector = m_unswitchCollectors.last();
    std::tuple noted { m_frames.last().function, read, list };
    if (collector.numericReads.contains(noted))
        return;
    collector.numericReads.append(noted);
    if (static_cast<size_t>(std::ranges::count_if(collector.numericReads, [&](const auto& other) { return std::get<Reference>(other) == list; })) < fewestReadsOffVersionedList)
        return;
    size_t place = collector.candidates.findIf([](const UnswitchCandidate& candidate) { return candidate.asksForInt32s; });
    if (place == notFound) {
        place = collector.candidates.size();
        collector.candidates.append({ });
        collector.candidates.last().asksForInt32s = true;
    }
    collector.candidates[place].storageAsked = ArrayStorage::Double;
}

void Walker::noteInt32Guard()
{
    VersionedLoop& versionedLoop = m_versionedLoops.last();
    versionedLoop.isUnswitched |= std::ranges::any_of(versionedLoop.unswitchCandidates, &UnswitchCandidate::asksForInt32s);
}

namespace {

class NumberElements {
public:
    NumberElements(Walker& walker, const BodyIndex& index)
        : m_walker(walker)
        , m_index(index)
    {
    }

    bool isElement(ValueID operand, uint32_t firstValueOfLoop)
    {
        ValueID element = operand ? m_index.stripCasts(operand) : ValueID();
        if (!element || element.index() < firstValueOfLoop || m_index.isParameter(element) || m_index.opcodeOf(element) != Opcode::ElemGet)
            return false;
        TypeID seen = m_walker.facts().linked().resultOfReadByKey(m_walker.currentFunction(), element);
        if (seen && !m_walker.types().overlap(seen, TypeTable::number))
            return false;
        ValueID list = m_index.value(element).objectOperand();
        return m_lists.ensure(list.index(), [&] { return isUnbuiltListOfNumbers(m_walker, list); }).iterator->value;
    }

private:
    Walker& m_walker;
    const BodyIndex& m_index;
    UncheckedKeyHashMap<uint32_t, bool, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lists;
};

} // anonymous namespace

static bool passesNumberOn(NumberElements& numbers, const BodyValue& operation, uint32_t firstValueOfLoop)
{
    static constexpr OpcodeSet passes { Opcode::Call, Opcode::PropSet, Opcode::PrivateSet, Opcode::ElemSet, Opcode::PutModuleVariable, Opcode::PutEnvironmentField };
    bool passesOn = false;
    if (passes.contains(operation.opcode())) {
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            passesOn = passesOn || numbers.isElement(operand, firstValueOfLoop);
        });
    }
    return passesOn;
}

static bool makesKeyOfNumber(Walker& walker, NumberElements& numbers, const BodyIndex& index, const BodyValue& access, uint32_t firstValueOfLoop)
{
    if (access.opcode() != Opcode::ElemGet && access.opcode() != Opcode::ElemSet)
        return false;
    ValueID key = index.stripCasts(findUpdatedKey(walker, index, access.propertyOperand()));
    if (!key || key.index() < firstValueOfLoop || index.isParameter(key) || !allBitwiseOperators.contains(index.opcodeOf(key)))
        return false;
    bool isMadeOfNumber = false;
    index.value(key).forEachOperand([&](ValueID operand, uint32_t) {
        isMadeOfNumber = isMadeOfNumber || numbers.isElement(operand, firstValueOfLoop);
    });
    return isMadeOfNumber;
}

static bool addsNumberToProperty(NumberElements& numbers, const BodyIndex& index, const BodyValue& operation, uint32_t firstValueOfLoop)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    static constexpr OpcodeSet reads { Opcode::PropGet, Opcode::ElemGet, Opcode::PrivateGet };
    static constexpr OpcodeSet bitwise { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor };
    static constexpr OpcodeSet stores { Opcode::PropSet, Opcode::ElemSet, Opcode::PrivateSet };
    if (!sums.contains(operation.opcode()))
        return false;
    ValueID sum = operation.id();
    ValueID user = index.onlyUsingValue(sum);
    if (user && bitwise.contains(index.opcodeOf(user))) {
        sum = user;
        user = index.onlyUsingValue(sum);
    }
    if (!user || !stores.contains(index.opcodeOf(user)) || index.stripCasts(index.value(user).valueOperand()) != sum)
        return false;
    auto sourceOf = [&](ValueID operand) {
        ValueID source = index.stripCasts(operand);
        return source && source.index() >= firstValueOfLoop && !index.isParameter(source) && reads.contains(index.opcodeOf(source)) ? source : ValueID();
    };
    ValueID left = sourceOf(operation.operand(0));
    ValueID right = sourceOf(operation.operand(1));
    return left && right && (numbers.isElement(right, firstValueOfLoop) || numbers.isElement(left, firstValueOfLoop));
}

static bool conditionMayBeInvariant(const BodyIndex& index, ValueID written, uint32_t firstValueOfLoop, unsigned depth, const Invocable<bool(uint32_t)> auto& parameterMayBeInvariant)
{
    ValueID value = index.stripCasts(written);
    if (!value)
        return false;
    if (firstValueOfLoop ? value.index() < firstValueOfLoop : index.isParameter(value) && index.blockOf(value) == entryBlock)
        return firstValueOfLoop || parameterMayBeInvariant(value.index());
    if (!depth || index.isParameter(value))
        return false;
    BodyValue operation = index.value(value);
    Opcode opcode = operation.opcode();
    if (primitiveConstants.contains(opcode) || opcode == Opcode::GetModuleVariable || opcode == Opcode::Builtin || opcode == Opcode::String)
        return true;
    static constexpr OpcodeSet operators = allRelations | allEqualities | OpcodeSet { Opcode::ToBoolean, Opcode::LogicalNot, Opcode::CheckInitialized, Opcode::TypeTest, Opcode::GetEnvironmentField, Opcode::PrivateGet, Opcode::PropGet, Opcode::ElemGet, Opcode::Call };
    if (opcode == Opcode::PropGet && operation.isFreeName())
        return true;
    bool mayBeInvariant = operators.contains(opcode);
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        mayBeInvariant = mayBeInvariant && (!operand || conditionMayBeInvariant(index, operand, firstValueOfLoop, depth - 1, parameterMayBeInvariant));
    });
    return mayBeInvariant;
}

static bool readsElementsOfList(const Facts& facts, FunctionID function, const BodyIndex& index, LoopID loop, uint32_t firstValueOfLoop)
{
    auto countReads = [](const BodyIndex& body, const auto& blocks, ValueID list) {
        size_t reads = 0;
        for (BlockID block : blocks) {
            for (BodyValue operation : body.block(block).values())
                reads += operation.opcode() == Opcode::ElemGet && body.stripCasts(operation.objectOperand()) == list;
        }
        return reads;
    };
    Vector<ValueID, 8> listsRead;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::ElemGet) {
                ValueID list = index.stripCasts(operation.objectOperand());
                if (list && list.index() < firstValueOfLoop && !listsRead.contains(list)) {
                    listsRead.append(list);
                    if (countReads(index, index.membersOf(loop), list) >= fewestReadsOffVersionedList)
                        return true;
                }
            }
            if (operation.opcode() != Opcode::Call)
                continue;
            FunctionID callee = facts.linked().calleeAt(function, operation.id());
            const BodyIndex* body = callee ? facts.bodies().find(callee) : nullptr;
            if (!body || !body->opcodes().contains(Opcode::ElemGet))
                continue;
            const Function& called = facts.program().at(callee);
            std::span<const ValueID> arguments = operation.argumentsOperands();
            BodyBlock entry = body->block(entryBlock);
            for (uint32_t place = 0; place < entry.numberOfParameters(); ++place) {
                uint32_t position = place - called.firstParameterIndex();
                if (called.entryParameterKind(place) != EntryParameter::Parameter || position >= arguments.size() || !arguments[position])
                    continue;
                ValueID argument = index.stripCasts(arguments[position]);
                if (!argument || argument.index() >= firstValueOfLoop)
                    continue;
                size_t reads = 0;
                for (uint32_t number = 0; number < body->numberOfBlocks() && reads < fewestReadsOffVersionedList; ++number)
                    reads += countReads(*body, std::array { BlockID(number) }, entry.parameter(place));
                if (reads >= fewestReadsOffVersionedList)
                    return true;
            }
        }
    }
    return false;
}

BitVector Walker::findLoopsToUnswitch(FunctionID function, const BodyIndex& index)
{
    BitVector found;
    static constexpr OpcodeSet elements { Opcode::ElemGet, Opcode::ElemSet };
    if (!index.holdsAnyOf(elements))
        return found;
    NumberElements numbers(*this, index);
    for (uint32_t number = 0; number < index.numberOfLoops(); ++number) {
        LoopID loop(number);
        BlockID head = index.loop(loop).head;
        if (!index.isReachable(head))
            continue;
        bool accessesElement = false;
        Vector<ValueID, 8> calls;
        uint32_t firstValueOfLoop = index.block(head).firstParameter().index();
        bool passesOn = false;
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values()) {
                accessesElement |= elements.contains(operation.opcode());
                passesOn = passesOn || passesNumberOn(numbers, operation, firstValueOfLoop) || addsNumberToProperty(numbers, index, operation, firstValueOfLoop) || makesKeyOfNumber(*this, numbers, index, operation, firstValueOfLoop);
                if (operation.opcode() == Opcode::Call)
                    calls.append(operation.id());
            }
        }
        if (passesOn)
            found.set(3 * number + 1);
        if (readsElementsOfList(facts(), function, index, loop, firstValueOfLoop))
            found.set(3 * number + 2);
        if (!accessesElement || calls.isEmpty())
            continue;
        auto noParameter = [](uint32_t) { return false; };
        auto mayUnswitchOn = [&](BlockID member) {
            BodyTerminator end = index.terminator(member);
            return member != head && end.kind() == TerminatorKind::Branch && conditionMayBeInvariant(index, end.as<TerminatorKind::Branch>().conditionOperand(), firstValueOfLoop, 8, noParameter) && quietTargetOf(index, end);
        };
        auto mayUnswitchInside = [&](ValueID site) {
            FunctionID callee = facts().linked().calleeAt(function, site);
            const BodyIndex* body = callee ? facts().bodies().find(callee) : nullptr;
            BodyTerminator begins = body ? body->terminator(entryBlock) : BodyTerminator();
            if (!body || begins.kind() != TerminatorKind::Branch)
                return false;
            const Function& called = program().at(callee);
            BodyValue call = index.value(site);
            auto argumentMayBeInvariant = [&](uint32_t parameter) {
                EntryParameter kind = called.entryParameterKind(parameter);
                uint32_t position = parameter - called.firstParameterIndex();
                ValueID argument = kind == EntryParameter::This ? call.thisValueOperand() : kind == EntryParameter::Parameter && position < call.argumentsOperands().size() ? call.argumentsOperands()[position] : ValueID();
                bool isCaptured = kind != EntryParameter::This && kind != EntryParameter::Parameter;
                return isCaptured || (argument && conditionMayBeInvariant(index, argument, firstValueOfLoop, 4, noParameter));
            };
            return conditionMayBeInvariant(*body, begins.as<TerminatorKind::Branch>().conditionOperand(), 0, 8, argumentMayBeInvariant) && quietTargetOf(*body, begins);
        };
        if (std::ranges::any_of(index.membersOf(loop), mayUnswitchOn) || std::ranges::any_of(calls, mayUnswitchInside))
            found.set(3 * number);
    }
    return found;
}

bool Walker::mayUnswitchLoop(BlockID inputHead, OtherClassBelowMixin& otherClass)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    if (m_coldCopyDepth || !m_opensGuesses || hasStringsInBuilders() || isUnreachable() || !has(Mechanism::LoopInvariants) || !has(Mechanism::EscapeAnalysis))
        return false;
    if (m_frames[frameIndex].loopsBeingVersioned.contains(loop) || m_reversionableLoops.contains(std::pair<size_t, LoopID> { frameIndex, loop }) || m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)))
        return false;
    if (!m_sharedGenericVersions.isEmpty() && m_sharedGenericVersions.last().frameIndex == frameIndex && m_sharedGenericVersions.last().inputHead == inputHead)
        return false;
    FunctionID function = m_frames[frameIndex].function;
    const BitVector& loopsToUnswitch = m_loopsToUnswitch.ensure(&index, [&] { return findLoopsToUnswitch(function, index); }).iterator->value;
    bool mayUnswitch = loopsToUnswitch.get(3 * loop.index()) || loopsToUnswitch.get(3 * loop.index() + 1) || loopsToUnswitch.get(3 * loop.index() + 2);
    bool holdsReaderLoop = false;
    if (program().someClassExtendsMixin) [[unlikely]] {
        BlockID innerMember;
        for (BlockID member : index.loop(loop).numberOfLoopsWithin == 2 ? index.membersOf(loop) : std::span<const BlockID>()) {
            if (index.loopOf(member) != loop)
                innerMember = member;
        }
        holdsReaderLoop = innerMember && callsReaders(*this, index.loop(index.loopOf(innerMember)).head);
        if (index.loop(loop).numberOfLoopsWithin == 1 || holdsReaderLoop)
            otherClass = findOtherClassBelowMixin(*this, inputHead);
    }
    return (mayUnswitch || otherClass.others) && mayBuildLoopSeveralTimes(inputHead) && (index.loop(loop).numberOfLoopsWithin == 1 || holdsReaderLoop || hasUnrolledInnerLoops(inputHead));
}

bool Walker::isQuietOnceUnswitched(LoopID loop, std::span<const ValueID> ranOtherCode, std::span<const UnswitchCandidate> candidates, Vector<ValueID, 4>& operationsOutsideCandidates) const
{
    const BodyIndex& index = currentBodyIndex();
    FunctionID function = m_frames.last().function;
    auto hasCandidateInCopyOf = [&](FunctionID callee) {
        return std::ranges::any_of(candidates, [&](const UnswitchCandidate& candidate) { return candidate.asksForInt32s || candidate.testsForOtherClass || (callee ? candidate.function == callee : candidate.function != function); });
    };
    for (ValueID ran : ranOtherCode) {
        if (!ran || ran.index() >= index.numberOfValues() || !index.isInLoop(index.blockOf(ran), loop))
            return false;
        bool isBehindCandidate = std::ranges::any_of(candidates, [&](const UnswitchCandidate& candidate) {
            return !candidate.asksForInt32s && !candidate.testsForOtherClass && candidate.function == function && index.dominates(index.terminator(candidate.block).targets()[1 - candidate.quietTarget].block(), index.blockOf(ran));
        });
        if (isBehindCandidate)
            continue;
        if (!index.isParameter(ran) && effectRuleOf(index.opcodeOf(ran)) == EffectRule::Call) {
            if (!hasCandidateInCopyOf(facts().linked().calleeAt(function, ran)))
                return false;
            continue;
        }
        static constexpr OpcodeSet mayBeTyped { Opcode::ElemGet, Opcode::ElemSet, Opcode::PropGet, Opcode::PropSet, Opcode::PrivateGet, Opcode::PrivateSet, Opcode::ResultAt, Opcode::IteratorResume, Opcode::ToPropertyKey };
        if (!index.isParameter(ran) && effectRuleOf(index.opcodeOf(ran)) != EffectRule::Operator && !mayBeTyped.contains(index.opcodeOf(ran)))
            return false;
        operationsOutsideCandidates.append(ran);
    }
    return true;
}

ValueID Walker::siteReadBehind(ValueID record) const
{
    const Operation& narrowed = m_code.at(record);
    if (narrowed.opcode != Opcode::Narrowed || m_code.isConstant(narrowed.operand(0)) || m_code.at(narrowed.operand(0)).opcode != Opcode::GetValueAtSiteOrAbsent || !narrowed.testOperand() || m_code.isConstant(narrowed.testOperand()))
        return { };
    const Operation& test = m_code.at(narrowed.testOperand());
    bool asksForAbsence = test.opcode == Opcode::TypeTest && test.testedType() == TypeArena::fromPrimitives(Primitive::Absent) && test.valueOperand() == narrowed.operand(0);
    return asksForAbsence && !narrowed.isOnTakenEdge() ? narrowed.operand(0) : ValueID();
}

Reference Walker::lookupThatAnswers(Reference value) const
{
    constexpr auto purity = IterationPurity::KnownEffectsOnly;
    std::optional<uint32_t> place = specializedOpenLoop(purity);
    if (!place || !value.isValue() || m_code.isConstant(value.id()) || m_openLoops[*place].firstReloadBlock || m_code.at(m_openLoops[*place].preheaderBlock).handler)
        return { };
    ValueID record = value.id();
    auto isNameOfOperand = [&](const Operation& narrowed) {
        return narrowed.opcode == Opcode::Narrowed && !narrowed.narrowsHolderOfOperand() && !narrowed.narrowsOtherResultOfOperand() && !m_code.isConstant(narrowed.operand(0));
    };
    for (; isNameOfOperand(m_code.at(record)); record = m_code.at(record).operand(0)) {
        const Operation& narrowed = m_code.at(record);
        if (narrowed.isOnTakenEdge() || !narrowed.testOperand() || m_code.isConstant(narrowed.testOperand()))
            continue;
        const Operation& test = m_code.at(narrowed.testOperand());
        Reference lookup = Reference::value(narrowed.operand(0));
        if (test.opcode == Opcode::TypeTest && test.testedType() == TypeArena::fromPrimitives(Primitive::Absent) && test.valueOperand() == lookup.id())
            return isInvariantInSpecializedVersion(lookup, purity) ? lookup : Reference();
    }
    return { };
}

Reference Walker::lookUpAfterLookup(Reference earlier, Draft&& lookup)
{
    constexpr auto purity = IterationPurity::KnownEffectsOnly;
    std::optional<uint32_t> place = specializedOpenLoop(purity);
    if (!place || !earlier.isValue())
        return { };
    Origin origin = lookup.origin();
    PropertyKeyID key = lookup.key();
    for (const OpenLoop::LookupAfterLookup& made : m_openLoops[*place].lookupsAfterLookups) {
        if (made.earlier == earlier.id() && made.key == key)
            return Reference::value(made.result);
    }
    SpecializedPreheaderScope preheaderScope(*this, true, purity);
    WalkState iterationState(m_state);
    m_state.clear();
    Reference found;
    Draft question(Opcode::TypeTest, origin);
    question.setValueOperand(earlier);
    question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
    Reference isAbsent = build(WTF::move(question));
    if (isAbsent && isAbsent.isValue() && !m_hasFailed) {
        BlockID front = m_block;
        BlockID present = m_code.addBlock(BlockRole::Ordinary, m_code.at(front).frequency);
        BlockID joined = m_code.addBlock(BlockRole::Ordinary, m_code.at(front).frequency);
        ValueID result = m_code.appendParameter(joined, TypeTable::anythingOrAbsent, Representation::JSValue, origin);
        m_code.setParameters(joined, singleElementSpan(result));
        while (m_preheaderDepths.size() <= joined.index())
            m_preheaderDepths.append(0);
        for (BlockID block : { present, joined })
            m_code.setLoopMarks(block, m_code.at(front).isInLoop, false);
        setPreheaderDepth(joined, *place + 1);
        m_code.endWithBranch(front, isAbsent.id(), origin);
        crossEdge(m_code.at(front).terminator.taken(), joined, singleElementSpan(earlier));
        m_code.closeEdge(m_code.at(front).terminator.notTaken(), present, { });
        m_block = present;
        SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
        SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, m_openLoops.size());
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(earlier);
        proof.setTestOperand(isAbsent);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(false);
        Reference answer = build(WTF::move(proof));
        if (answer && answer.isRecord() && !m_hasFailed) {
            lookup.setObjectOperand(answer);
            answer = build(WTF::move(lookup));
        }
        if (answer && !m_hasFailed) {
            crossEdge({ }, joined, singleElementSpan(answer));
            OpenLoop& loop = m_openLoops.changeable(*place);
            loop.earlierPreheaderBlocks.append(front);
            loop.preheaderBlock = joined;
            loop.lookupsAfterLookups.append({ earlier.id(), key, result });
            found = Reference::value(result);
        }
    }
    m_state = WTF::move(iterationState);
    return found;
}

bool Walker::canHoistSiteRead(uint32_t openLoop, ValueID record, unsigned depth, UnswitchedIterations iterations) const
{
    const Operation& read = m_code.at(record);
    const Effects& ofIterations = m_openLoops[openLoop].effects;
    if (iterations == UnswitchedIterations::AsWalked && (ofIterations.mayRunOtherCode() || ofIterations.writes.overlaps(declaredBoundInTable(Opcode::GetValueAtSiteOrAbsent).reads.without(unobservableWriteHeaps))))
        return false;
    ValueID object = read.objectOperand();
    const Operation& holder = m_code.at(object);
    if (!m_code.isConstant(object) && holder.opcode == Opcode::GetBuiltinValue)
        return !holder.traits().mayRunOtherCode() && !isUnderHandler();
    return canHoistUnswitchRead(openLoop, object, depth - 1, iterations);
}

bool Walker::canHoistUnswitchRead(uint32_t openLoop, ValueID record, unsigned depth, UnswitchedIterations iterations) const
{
    if (m_code.isConstant(record) || isInvariantInLoop(openLoop, Reference::value(record)))
        return true;
    const Operation& made = m_code.at(record);
    if (!depth)
        return false;
    if (ValueID read = siteReadBehind(record))
        return canHoistSiteRead(openLoop, read, depth, iterations);
    if (conversionsOfForm.contains(made.opcode))
        return canHoistUnswitchRead(openLoop, made.operand(0), depth - 1, iterations);
    if (made.opcode != Opcode::GetSlot || made.level() != KnowledgeLevel::Proven)
        return false;
    LocationMasks reads;
    reads.add(program(), MemoryLocation::objectField(made.key()));
    bool isKept = iterations == UnswitchedIterations::AreQuiet || !reads.overlaps(m_openLoops[openLoop].effects.writes);
    return isKept && canHoistUnswitchRead(openLoop, made.objectOperand(), depth - 1, iterations);
}

static constexpr OpcodeSet unaryUnswitchOperators { Opcode::ToBoolean, Opcode::LogicalNot, Opcode::TypeTest };
static constexpr OpcodeSet unswitchOperators = allRelations | allEqualities | unaryUnswitchOperators;

bool Walker::canHoistUnswitchCondition(uint32_t openLoop, ValueID written, unsigned depth, UnswitchedIterations iterations) const
{
    const BodyIndex& index = currentBodyIndex();
    Reference bound = referenceBoundTo(written);
    if (!bound)
        return false;
    if (bound.isConstant() || (bound.isValue() && canHoistUnswitchRead(openLoop, bound.id(), depth, iterations)))
        return true;
    ValueID value = index.stripCasts(written);
    if (!value || !depth || index.isParameter(value) || !unswitchOperators.contains(index.opcodeOf(value)))
        return false;
    BodyValue operation = index.value(value);
    return canHoistUnswitchCondition(openLoop, operation.operand(0), depth - 1, iterations) && (unaryUnswitchOperators.contains(operation.opcode()) || canHoistUnswitchCondition(openLoop, operation.operand(1), depth - 1, iterations));
}

Reference Walker::buildUnswitchRead(uint32_t openLoop, ValueID record, unsigned depth)
{
    if (m_code.isConstant(record))
        return Reference::constant(record);
    if (isInvariantInLoop(openLoop, Reference::value(record)))
        return Reference::value(record);
    const Operation& made = m_code.at(record);
    if (!depth || m_openLoops[openLoop].effects.mayRunOtherCode())
        return { };
    if (ValueID read = siteReadBehind(record)) {
        size_t hoisted = m_versionedLoops.last().hoistedSiteReads.findIf([&](const HoistedSiteRead& other) { return other.read == read; });
        return hoisted == notFound ? Reference() : m_versionedLoops.last().hoistedSiteReads[hoisted].value;
    }
    if (conversionsOfForm.contains(made.opcode))
        return buildUnswitchRead(openLoop, made.operand(0), depth - 1);
    LocationMasks reads;
    if (made.opcode == Opcode::GetSlot)
        reads.add(program(), MemoryLocation::objectField(made.key()));
    if (made.opcode != Opcode::GetSlot || made.level() != KnowledgeLevel::Proven || reads.overlaps(m_openLoops[openLoop].effects.writes))
        return { };
    Draft read(Opcode::GetSlot, m_origin);
    chooseConservativeFields(read);
    read.setKey(made.key());
    read.setReceivers(made.receivers());
    read.setLevel(KnowledgeLevel::Proven);
    Reference object = buildUnswitchRead(openLoop, made.objectOperand(), depth - 1);
    if (!object || m_hasFailed || !m_types.isSubset(typeOf(object), m_code.at(made.objectOperand()).type))
        return { };
    read.setObjectOperand(object);
    return build(WTF::move(read));
}

Reference Walker::buildUnswitchCondition(uint32_t openLoop, ValueID written, unsigned depth)
{
    const BodyIndex& index = currentBodyIndex();
    Reference bound = referenceOf(written);
    if (!bound || m_hasFailed)
        return { };
    if (bound.isRecord() && typeOf(bound) == ownTypeOf(bound)) {
        if (isInvariantInLoop(openLoop, bound))
            return bound;
        if (Reference read = bound.isValue() ? buildUnswitchRead(openLoop, bound.id(), depth) : Reference(); read || m_hasFailed)
            return read;
    }
    ValueID value = index.stripCasts(written);
    if (!value || !depth || index.isParameter(value))
        return { };
    BodyValue operation = index.value(value);
    if (!unswitchOperators.contains(operation.opcode()))
        return { };
    Reference left = buildUnswitchCondition(openLoop, operation.operand(0), depth - 1);
    bool isBinary = !unaryUnswitchOperators.contains(operation.opcode());
    Reference right = left && isBinary ? buildUnswitchCondition(openLoop, operation.operand(1), depth - 1) : Reference();
    if (!left || (isBinary && !right) || m_hasFailed)
        return { };
    if (operation.opcode() == Opcode::ToBoolean && isBooleanType(typeOf(left)))
        return left;
    if (operation.opcode() == Opcode::TypeTest) {
        Draft test(Opcode::TypeTest, m_origin);
        test.setValueOperand(left);
        test.setTestedType(operation.testedType());
        return build(WTF::move(test));
    }
    return buildOperatorForStrings(*this, operation.opcode(), left, right, m_origin);
}

void Walker::hoistSiteReads(uint32_t openLoop, ValueID written, unsigned depth)
{
    constexpr auto purity = IterationPurity::KnownEffectsOnly;
    Reference bound = referenceOf(written);
    if (!bound || m_hasFailed || isUnreachable())
        return;
    ValueID read = bound.isValue() && !m_code.isConstant(bound.id()) ? siteReadBehind(bound.id()) : ValueID();
    if (!read) {
        const BodyIndex& index = currentBodyIndex();
        ValueID value = index.stripCasts(written);
        if (!value || !depth || index.isParameter(value) || !unswitchOperators.contains(index.opcodeOf(value)))
            return;
        index.value(value).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand)
                hoistSiteReads(openLoop, operand, depth - 1);
        });
        return;
    }
    if (!canHoistSiteRead(openLoop, read, depth) || isInvariantInLoop(openLoop, bound) || m_versionedLoops.last().hoistedSiteReads.containsIf([&](const HoistedSiteRead& other) { return other.read == read; }))
        return;
    Origin origin = m_origin;
    origin.isTailCall = false;
    Reference found;
    Reference isAbsent;
    {
        SpecializedPreheaderScope preheaderScope(*this, true, purity);
        WalkState iterationState(m_state);
        m_state.clear();
        ValueID holder = m_code.at(read).objectOperand();
        bool isLazyBuiltin = !m_code.isConstant(holder) && m_code.at(holder).opcode == Opcode::GetBuiltinValue;
        Reference object = isLazyBuiltin ? buildLazyBuiltinValue(*this, m_code.at(holder).builtin(), m_code.at(holder).overload(), origin) : buildUnswitchRead(openLoop, holder, depth - 1);
        if (object && object.isRecord() && !m_hasFailed) {
            Draft lookup(Opcode::GetValueAtSiteOrAbsent, origin);
            lookup.setObjectOperand(object);
            lookup.setKey(m_code.at(read).key());
            lookup.setReceiverIsNoObjectOfCompiledCode(m_code.at(read).receiverIsNoObjectOfCompiledCode());
            lookup.setValueType(TypeTable::anythingOrAbsent);
            found = build(WTF::move(lookup));
        }
        if (found && found.isValue() && !m_hasFailed) {
            Draft question(Opcode::TypeTest, origin);
            question.setValueOperand(found);
            question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
            isAbsent = build(WTF::move(question));
        }
        m_state = WTF::move(iterationState);
    }
    if (!isAbsent || !isAbsent.isValue() || m_hasFailed || !tryBranchInPreheader(isAbsent, false, purity, GuardedIterations::Some))
        return;
    SpecializedPreheaderScope preheaderScope(*this, true, purity);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(found);
    proof.setTestOperand(isAbsent);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(false);
    if (Reference value = build(WTF::move(proof)); value && !m_hasFailed)
        m_versionedLoops.last().hoistedSiteReads.append({ read, value });
}

Reference Walker::hoistUnswitchCondition(uint32_t openLoop, ValueID written)
{
    m_versionedLoops.last().hoistedSiteReads.shrink(0);
    hoistSiteReads(openLoop, written, 6);
    if (m_hasFailed || isUnreachable())
        return { };
    SpecializedPreheaderScope preheaderScope(*this, true, IterationPurity::KnownEffectsOnly);
    WalkState iterationState(m_state);
    m_state.clear();
    Reference condition = buildUnswitchCondition(openLoop, written, 6);
    if (condition && condition.isValue() && !m_hasFailed && representationOf(condition) != Representation::Boolean)
        condition = isBooleanType(typeOf(condition)) ? convert(condition, Representation::Boolean, m_origin) : Reference();
    m_state = WTF::move(iterationState);
    return condition && condition.isValue() && !m_hasFailed && isInvariantInLoop(openLoop, condition) ? condition : Reference();
}

void Walker::cutUnswitchedWalkShort()
{
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    for (size_t place = m_trialVisits.size(); place--;) {
        if (m_trialVisits[place].frameIndex != versionedLoop.frameIndex || m_trialVisits[place].inputHead != versionedLoop.inputHead)
            continue;
        if (!m_loopSummaries[m_trialVisits[place].summaryIndex].trialVisitCount)
            return;
        m_hasUnswitchFailed = true;
        m_numberOfTryWhoseWalkIsCutShort = static_cast<uint32_t>(place) + 1;
        return;
    }
}

bool Walker::tryUnswitchBranch(const BodyTerminator& branch)
{
    for (const UnswitchCandidate& candidate : m_versionedLoops.last().unswitchCandidates) {
        if (candidate.asksForInt32s || candidate.testsForOtherClass || candidate.function != m_frames.last().function || candidate.block != branch.block())
            continue;
        uint32_t quietTarget = candidate.quietTarget;
        std::optional<uint32_t> openLoop = specializedOpenLoop(IterationPurity::KnownEffectsOnly);
        if (!openLoop || m_code.at(m_block).frequency != BlockFrequency::Normal)
            return false;
        Reference condition = hoistUnswitchCondition(*openLoop, branch.as<TerminatorKind::Branch>().conditionOperand());
        if (m_hasFailed)
            return true;
        if (!condition || !tryBranchInPreheader(condition, !quietTarget, IterationPurity::KnownEffectsOnly, GuardedIterations::Some)) {
            cutUnswitchedWalkShort();
            return walkIsCutShortHere();
        }
        m_versionedLoops.last().isUnswitched = true;
        count(StatisticID::BranchesUnswitched);
        EdgeToTargetOfBody quiet { branch.block(), quietTarget };
        endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(quiet));
        return true;
    }
    return false;
}

bool Walker::someIterationLeavesStringsUnchanged(BlockID inputHead, std::span<const GrowingText> texts)
{
    const BodyIndex& index = currentBodyIndex();
    if (stepsThroughList(*this, index, inputHead))
        return false;
    Vector<ValueID, 8> passedBy;
    for (const GrowingText& text : texts) {
        if (!text.parametersOfInnerHeads.isEmpty())
            return false;
        for (const NameOfGrowingText& name : text.names) {
            if (!index.isParameter(name.value))
                passedBy.append(name.value);
        }
    }
    return blocksThatSomeQuietTurnPassesThrough(inputHead, passedBy.span()).get(inputHead.index());
}

bool Walker::someTextGrowsInQuietTurn(BlockID inputHead, std::span<const GrowingText> texts, const TrialVisit& trialVisit)
{
    const BodyIndex& index = currentBodyIndex();
    if (stepsThroughList(*this, index, inputHead))
        return true;
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
    if (trialVisit.carriesElement)
        written.merge(trialVisit.writtenBeforeIterationRunsAgain);
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

bool Walker::isIterationQuiet() const
{
    std::optional<uint32_t> loop = innermostOpenLoop();
    return loop && mayReadInFrontOfQuietTurns(*loop);
}

bool Walker::isReadForCall(const Draft& draft) const
{
    if (draft.opcode != Opcode::GetModuleVariable || !isWrittenInTopFrame(draft))
        return false;
    const BodyIndex& index = currentBodyIndex();
    ValueID taker = index.onlyUsingValue(draft.inputValueGiven);
    while (taker && (inputPassThroughOpcodes.contains(index.opcodeOf(taker)) || index.opcodeOf(taker) == Opcode::CheckInitialized))
        taker = index.onlyUsingValue(taker);
    if (!taker || index.opcodeOf(taker) != Opcode::Call)
        return false;
    const auto& linked = facts().linked();
    FunctionID callee = linked.calls.isEmpty() ? FunctionID() : linked.calleeAt(currentFunction(), taker);
    return callee && linked.function(callee).effectsOnReturn().mayRunOtherCode();
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

std::optional<uint32_t> Walker::loopWhoseIterationMayRunAgainBehindReads() const
{
    std::optional<uint32_t> place = specializedOpenLoop(IterationPurity::EffectFree);
    if (!place || m_trialVisits.isEmpty() || m_openLoops[*place].frame != m_frames.size() - 1 || handlerPreventsIterationRestart() || !m_mergedHeaderObjects.isEmpty())
        return std::nullopt;
    const OpenLoop& loop = m_openLoops[*place];
    if ((!loop.firstReloadBlock && !loop.watchesTurns) || loop.summaryIndexPlusOne != m_trialVisits.last().summaryIndex + 1 || !mayReadInFrontOfQuietTurns(*place))
        return std::nullopt;
    return place;
}

bool Walker::buildGrowthOfList(Draft&& growth)
{
    std::optional<uint32_t> place = loopWhoseIterationMayRunAgainBehindReads();
    SetForScope runsAgain(m_iterationRunsAgainBehindGrowth, !!place);
    build(WTF::move(growth));
    return place && m_openLoops[*place].firstReloadBlock;
}

void Walker::runIterationAgainBehindReads()
{
    if (m_hasFailed || isUnreachable())
        return;
    std::optional<uint32_t> place = loopWhoseIterationMayRunAgainBehindReads();
    if (!place || !m_openLoops[*place].firstReloadBlock) {
        fail("an iteration whose list has grown cannot run again"_s);
        return;
    }
    BlockID head = m_openLoops[*place].head;
    Vector<Reference, 8> valuesAtHead;
    for (ValueID parameter : m_code.at(head).parameters())
        valuesAtHead.append(Reference::value(parameter));
    OpenLoop::EdgeHeldBack held;
    if (!makeReadyToCross(head, valuesAtHead.span(), true, held.arguments))
        return;
    m_code.end(m_block, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
    held.edge = m_code.at(m_block).terminator.next();
    m_openLoops.changeable(*place).edgesThatRunIterationAgain.append(WTF::move(held));
    m_block = { };
    count(StatisticID::GrownListsRunIterationAgain);
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
    for (const OpenLoop::EdgeHeldBack& held : loop.edgesThatRunIterationAgain)
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
