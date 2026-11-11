/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTFoldingOfConstants.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTResultRules.h"
#include "AOTSweep.h"
#include "AOTTypeNarrowing.h"
#include "AOTWalkKnown.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t mostOperationsOfJoinWalkedForEachWay = 8;

static constexpr OpcodeSet operationsWalkedOnceInKnownThreading = opcodesOfFamily(OperationFamily::Iteration) | opcodesOfFamily(OperationFamily::ForIn) | opcodesOfFamily(OperationFamily::Coroutine)
    | opcodesOfFamily(OperationFamily::Exception) | opcodesWhere([](Opcode opcode) { return boundToOf(opcode) != BoundTo::Nothing; }) | OpcodeSet { Opcode::LoopHint };

static constexpr OpcodeSet numericAndRelationalOperators = allRelations | allNumericOperators;
static constexpr OpcodeSet pathSensitiveOperations = numericAndRelationalOperators
    | OpcodeSet { Opcode::ToBoolean, Opcode::PropGet, Opcode::PropSet, Opcode::ElemGet, Opcode::ElemSet, Opcode::Call, Opcode::Construct };
static constexpr OpcodeSet operationsThatBuildNothingInKnownThreading = opcodesThatStateTypes | primitivesWrittenForFolding;

static bool mayBeWalkedAgainInKnownThreading(const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (operationsWalkedOnceInKnownThreading.contains(opcode) || maySuspendAsWritten(operation))
        return false;
    return opcode != Opcode::Convert || !operation.as<Opcode::Convert>().declared().has(PositionFlags::IsInt32Promise);
}

template<typename User>
static bool namesByDefinitionInKnownThreading(const User& user, ValueID value)
{
    bool namesByDefinition = false;
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        namesByDefinition |= operand == value && mustBeDefinition(user.operandKind(operandIndex));
    });
    return namesByDefinition;
}

static bool mustBeNamedByDefinitionInKnownThreading(const BodyIndex& index, ValueID value)
{
    std::optional<BodyUser> user = index.firstUser(value);
    if (!user)
        return false;
    switch (user->kind) {
    case BodyUseKind::Operand:
        return namesByDefinitionInKnownThreading(index.value(user->value), value);
    case BodyUseKind::TerminatorOperand:
        return namesByDefinitionInKnownThreading(index.terminator(user->block), value);
    case BodyUseKind::BlockSubject:
        return true;
    case BodyUseKind::EdgeArgument:
    case BodyUseKind::EdgeProof:
    case BodyUseKind::HandlerArgument:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class KnownThreading {
    WTF_MAKE_NONCOPYABLE(KnownThreading);
public:
    KnownThreading(Walker& walker, BlockID inputBlock)
        : KnownThreading(walker, walker.m_frames.size() - 1, inputBlock)
    {
    }
    KnownThreading(Walker& walker, size_t frameIndex, BlockID inputBlock)
        : m_walker(walker)
        , m_frameIndex(frameIndex)
        , m_index(*walker.m_frames[frameIndex].index)
        , m_block(m_index.block(inputBlock))
    {
    }

    using ReturnOfCopy = Walker::Frame::ReturnOfCopy;

    bool endArmWithBranch();
    bool armsMayWalkEndOfBlockFrom(uint32_t begin);
    bool walkApartFromClosedWays();
    bool walkApartFromWaysThatDidSomething();
    bool walkForEachWayThatBringsObjectInSight();
    bool walkApartBySidesOfQuestionAskedBefore();
    bool armsWouldStayApartBehindEndOfBlock(uint32_t begin, uint32_t end, Reference value, const BitVector& placesThatTypedRunKnowsBetter);
    bool isMentionedBehindBlock(ValueID);
    bool letReturnsOfCopyWalkEndOfBlockApart(ValueID call, std::span<ReturnOfCopy* const> passing, std::span<ReturnOfCopy* const> closed);
    const Walker::OpenLoop* loopWhoseQuietWaysGoToHead();
    bool letQuietReturnsOfCopyWalkEndOfBlockApart(ValueID call);
    bool letAnswersOfCopyWalkEndOfBlockApart(ValueID call);

private:
    Walker::Frame& frame() { return m_walker.m_frames[m_frameIndex]; }
    bool isFrameRecursive() { return m_walker.isFrameRecursive(frame()); }
    uint32_t firstValue() const { return m_block.firstValue().index(); }
    bool operationsMayBeWalkedAgain(uint32_t begin, uint32_t end);
    bool terminatorMayBeWalkedAgain();
    bool letTerminatorBeWalkedForEachWay(ValueID firstBuiltOnEachWay);
    bool mayBeWalkedApartFromClosedWays();
    bool isWhereAllSidesOfOneChoiceMeet();
    bool terminatorMayBeWalkedByWaysApart();
    struct PlaceLookedAt {
        size_t frameIndex { 0 };
        BlockID block;
        uint32_t begin { 0 };
        ValueID call;
        ValueID given;
    };
    struct LookAhead {
        Vector<PlaceLookedAt, 16> places;
        uint32_t numberOfPlacesAsked { 0 };
        ValueID bound;
        Vector<std::pair<ValueID, ValueID>, 8> brought;
    };
    ValueID valueFromInFront(const LookAhead&, ValueID operand);
    void lookBehindEdge(LookAhead&, const PlaceLookedAt& from, const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    void lookBehindReturnOfCopy(LookAhead&, const PlaceLookedAt& from, const BodyTerminator&);
    template<typename Functor> bool looksAhead(LookAhead&, const Functor& isLeanedOn);
    bool calleeAsksFirstWhatItIsHanded(const BodyValue& operation, uint32_t operandIndex, std::optional<bool>& copyOfCalleePays);
    bool endOfBlockMayBeWalkedForEachWayFrom(uint32_t begin);
    bool mayThrowFrom(uint32_t begin);
    uint32_t numberOfNamesBroughtApart(std::span<const uint32_t> ways);
    bool leansOnWhatWaysHaveProven(std::span<const uint32_t> passing, std::span<const uint32_t> closed);
    bool takesApartParameter(ValueID parameter);
    bool takesParameterAsKey(ValueID parameter);
    bool callsParameter(ValueID parameter, uint32_t numberOfBlocksLookedThrough = 0);
    bool letArmsWalkEndOfBlockFrom(uint32_t begin);
    void forgetValuesThatNobodyReadsAnyMore();
    void forgetValuesFrom(ValueID first);
    bool isReadBehindBlock(ValueID);

    uint32_t numberOfOperationsBuiltIn(uint32_t begin, uint32_t end, uint32_t enough);

    void walkBlockOnWays(std::span<const uint32_t> places, uint32_t namesAtHead);

    struct WaysIn {
        Vector<uint32_t, 4> places;
        uint32_t numberLedByClosedArms { 0 };
        bool allMayBeLed { true };
        bool isGathered { false };
    };
    const WaysIn& waysIn();

    Walker& m_walker;
    size_t m_frameIndex;
    const BodyIndex& m_index;
    BodyBlock m_block;
    uint32_t m_begin { 0 };
    WaysIn m_waysIn;
};

bool KnownThreading::operationsMayBeWalkedAgain(uint32_t begin, uint32_t end)
{
    for (uint32_t place = begin; place < end; ++place) {
        if (!mayBeWalkedAgainInKnownThreading(m_index.value(ValueID(firstValue() + place))))
            return false;
    }
    return true;
}

bool KnownThreading::terminatorMayBeWalkedAgain()
{
    if (m_walker.isInLoop() && boundsProvenOnEdge(m_index, m_block.terminator(), 0))
        return false;
    for (BodyTarget target : m_block.terminator().targets()) {
        BlockID to = target.block();
        if (to == m_block.id() || m_index.isLoopHead(to) || m_index.isEnteredByHandlerEdge(to) || m_index.block(to).isHandlerBlock() || m_walker.inputBlock(m_frameIndex, to).isEntered)
            return false;
    }
    return true;
}

ALWAYS_INLINE void KnownThreading::forgetValuesThatNobodyReadsAnyMore()
{
    if (!frame().loopsBeingVersioned.isEmpty() || !frame().duplicatedValues || m_walker.m_trialVisitDepth)
        return;
    uint32_t here = m_block.firstParameter().index();
    Walker::Frame::DuplicatedValues& listed = *frame().duplicatedValues;
    for (uint32_t slot : listed.slotsListedAnew) {
        if (listed.slots[slot].value.index() >= frame().index->numberOfValues())
            continue;
        listed.slotsByLastUse.append({ m_walker.m_liveness.lastUse(listed.slots[slot].value).index(), slot });
        std::ranges::push_heap(listed.slotsByLastUse, std::ranges::greater());
    }
    listed.slotsListedAnew.shrink(0);
    while (!listed.slotsByLastUse.isEmpty() && listed.slotsByLastUse.first().first < here) {
        std::ranges::pop_heap(listed.slotsByLastUse, std::ranges::greater());
        uint32_t dead = listed.slotsByLastUse.takeLast().second;
        if (!listed.slots[dead].ordinal)
            continue;
        listed.slots.mutableAt(dead).ordinal = 0;
        frame().references.placeOf<Walker::NamesOfValues::Writer::ForgetsValuesThatNobodyReadsAnyMore>(listed.slots[dead].value.index()) = Reference();
    }
}

void KnownThreading::forgetValuesFrom(ValueID first)
{
    for (uint32_t value = first.index(); value < m_block.endOfValues().index(); ++value)
        frame().references.placeOf<Walker::NamesOfValues::Writer::ForgetsValuesOfBlockWalkedApart>(value) = Reference();
}

bool KnownThreading::isReadBehindBlock(ValueID value)
{
    for (BodyTarget target : m_block.terminator().targets()) {
        if (m_walker.m_liveness.isLiveAtHead(value, target.block()))
            return true;
    }
    return false;
}

bool KnownThreading::letTerminatorBeWalkedForEachWay(ValueID firstBuiltOnEachWay)
{
    if (frame().blocksBeingTailDuplicated.contains(m_block.id()))
        return true;
    BodyLiveness& liveness = m_walker.m_liveness;
    if (!liveness.isComputedFor(m_index))
        liveness.compute(m_index);

    Vector<ValueID, 8> readBehind;
    for (BodyTarget target : m_block.terminator().targets()) {
        liveness.forEachLiveAtHead(target.block(), firstBuiltOnEachWay, m_block.endOfValues(), [&](ValueID value) {
            readBehind.append(value);
        });
    }
    Vector<ValueID, 4> wouldBeNamedForGood;
    if (frame().duplicatedValues) {
        for (const auto& given : frame().duplicatedValues->namesGivenForGood) {
            for (ValueID value : given.values) {
                if (given.block == m_block.id() && isReadBehindBlock(value))
                    wouldBeNamedForGood.append(value);
            }
        }
    }
    readBehind.appendVector(wouldBeNamedForGood);
    std::ranges::sort(readBehind);
    readBehind.shrink(std::ranges::unique(readBehind).begin() - readBehind.begin());

    for (ValueID value : readBehind) {
        if (!m_index.isParameter(value) && mustBeNamedByDefinitionInKnownThreading(m_index, value))
            return false;
    }
    Vector<ValueID, 8> readByHandlers;
    uint32_t lastUse = 0;
    for (ValueID value : readBehind) {
        if (!m_walker.numberOfHandlersThatRead(frame(), value))
            continue;
        readByHandlers.append(value);
        lastUse = std::max(lastUse, liveness.lastUse(value).index());
    }
    if (!readByHandlers.isEmpty()) {
        std::span<const BlockID> handlerBlocks = m_walker.handlerBlocksOf(frame());
        for (auto handlerBlock = std::ranges::upper_bound(handlerBlocks, m_block.id()); handlerBlock != handlerBlocks.end() && m_index.block(*handlerBlock).firstParameter().index() <= lastUse; ++handlerBlock) {
            for (ValueID value : readByHandlers) {
                if (liveness.isLiveAtHead(value, *handlerBlock))
                    return false;
            }
        }
    }

    forgetValuesThatNobodyReadsAnyMore();
    if (frame().duplicatedValues) {
        frame().duplicatedValues->namesGivenForGood.removeAllMatching([&](const auto& given) {
            return given.block == m_block.id();
        });
    }
    for (ValueID value : wouldBeNamedForGood)
        frame().duplicatedValues->remove(value);
    for (ValueID value : readBehind)
        frame().addDuplicatedValue(value);
    frame().blocksBeingTailDuplicated.shrink(0);
    frame().blocksBeingTailDuplicated.append(m_block.id());
    return true;
}

bool KnownThreading::letArmsWalkEndOfBlockFrom(uint32_t begin)
{
    return endOfBlockMayBeWalkedForEachWayFrom(begin) && letTerminatorBeWalkedForEachWay(m_block.firstValue());
}

bool KnownThreading::mayThrowFrom(uint32_t begin)
{
    for (uint32_t place = begin; place < m_block.numberOfValues(); ++place) {
        Opcode opcode = m_index.opcodeOf(ValueID(firstValue() + place));
        const Effects& bound = declaredBoundInTable(opcode);
        if (opcode == Opcode::Builtin || operandsMayRunCode(opcode) || bound.mayExitSideways() || bound.mayCollect)
            return true;
    }
    return false;
}

bool KnownThreading::endOfBlockMayBeWalkedForEachWayFrom(uint32_t begin)
{
    uint32_t numberOfOperations = m_block.numberOfValues();
    if (begin > numberOfOperations || numberOfOperations - begin > mostOperationsOfJoinWalkedForEachWay)
        return false;
    TerminatorKind end = m_block.terminator().kind();
    bool returnsFromCopy = (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall) && m_frameIndex && !frame().returnsInPlaceOfUnit;
    bool mayBeTaken = goesByOneOperand(end) || end == TerminatorKind::Jump || returnsFromCopy;
    if (!mayBeTaken || m_index.hasIrreducibleFlow() || (m_block.hasHandler() && (returnsFromCopy || mayThrowFrom(begin))))
        return false;
    return operationsMayBeWalkedAgain(begin, numberOfOperations) && terminatorMayBeWalkedAgain();
}

uint32_t KnownThreading::numberOfOperationsBuiltIn(uint32_t begin, uint32_t end, uint32_t enough)
{
    uint32_t number = 0;
    for (uint32_t place = begin; place < end && number <= enough; ++place)
        number += !operationsThatBuildNothingInKnownThreading.contains(m_index.opcodeOf(ValueID(firstValue() + place)));
    return number;
}

const KnownThreading::WaysIn& KnownThreading::waysIn()
{
    if (m_waysIn.isGathered)
        return m_waysIn;
    m_waysIn.isGathered = true;
    for (uint32_t place = m_walker.inputBlock(m_frameIndex, m_block.id()).firstOpenEdge; place != Walker::noOpenEdge; place = m_walker.m_openEdges[place].next) {
        const Walker::OpenEdge& open = m_walker.m_openEdges[place];
        if (open.block || open.arguments.size() != m_block.numberOfParameters()) {
            m_waysIn.allMayBeLed = false;
            break;
        }
        m_waysIn.numberLedByClosedArms += open.isLedByClosedArm;
        m_waysIn.places.append(place);
    }
    return m_waysIn;
}

void KnownThreading::walkBlockOnWays(std::span<const uint32_t> places, uint32_t namesAtHead)
{
    Walker& walker = m_walker;
    if (places.empty() || walker.m_hasFailed)
        return;
    Walker::InputBlock& record = walker.inputBlock(m_frameIndex, m_block.id());
    record = Walker::InputBlock { };
    record.firstNarrowerName = namesAtHead;
    for (uint32_t place : places)
        walker.m_openEdges[place].next = std::exchange(record.firstOpenEdge, place);
    frame().block = { };
    walker.walkBlock(m_block.id());
}

bool KnownThreading::armsMayWalkEndOfBlockFrom(uint32_t begin)
{
    return !m_walker.standsInForValueOfFrame() && frame().block == m_block.id() && frame().endOfStretch == m_block.numberOfValues() && letArmsWalkEndOfBlockFrom(begin);
}

bool KnownThreading::endArmWithBranch()
{
    Walker& walker = m_walker;
    uint32_t numberOfOperations = m_block.numberOfValues();
    m_begin = frame().walkedUpTo;
    if (frame().block != m_block.id() || frame().endOfStretch != numberOfOperations || !letArmsWalkEndOfBlockFrom(m_begin))
        return false;

    walker.walkStretch(m_block.id(), m_begin, numberOfOperations);
    frame().walkedUpTo = m_begin;
    walker.walkTerminator(m_block.terminator());
    forgetValuesFrom(ValueID(firstValue() + m_begin));
    return true;
}

static constexpr uint32_t mostBlocksLookedThroughInKnownThreading = 3;

bool KnownThreading::mayBeWalkedApartFromClosedWays()
{
    BlockID block = m_block.id();
    if (m_index.isLoopHead(block) || m_block.isHandlerBlock() || m_index.isEnteredByHandlerEdge(block) || m_block.hasHandler() || m_index.hasIrreducibleFlow())
        return false;
    TerminatorKind end = m_block.terminator().kind();
    if (end != TerminatorKind::Return && end != TerminatorKind::ReturnCall && !goesByOneOperand(end) && end != TerminatorKind::Jump)
        return false;
    uint32_t numberOfOperations = m_block.numberOfValues();
    return numberOfOperationsBuiltIn(0, numberOfOperations, mostOperationsOfJoinWalkedForEachWay) <= mostOperationsOfJoinWalkedForEachWay && operationsMayBeWalkedAgain(0, numberOfOperations);
}

bool KnownThreading::isWhereAllSidesOfOneChoiceMeet()
{
    BlockID choosing = m_index.immediateDominator(m_block.id());
    uint32_t numberOfWaysIn = m_index.numberOfIncomingEdges(m_block.id());
    if (!choosing || numberOfWaysIn == BodyIndex::maxCountedIncomingEdges)
        return false;
    BodyTerminator choice = m_index.terminator(choosing);
    auto forEachBlockWhereSideArrives = [&](const auto& functor) {
        for (BodyTarget target : choice.targets()) {
            BlockID side = target.block();
            BodyTerminator end = m_index.terminator(side);
            if (m_index.numberOfIncomingEdges(side) == 1 && end.kind() == TerminatorKind::Jump)
                functor(end.targets()[0].block());
        }
    };
    uint32_t numberOfSidesThatArrive = 0;
    if (choice.numberOfTargets() <= 2) {
        forEachBlockWhereSideArrives([&](BlockID block) {
            numberOfSidesThatArrive += block == m_block.id();
        });
        return numberOfSidesThatArrive == numberOfWaysIn;
    }
    const Walker::SidesThatArriveAtBlocks& arrivals = m_walker.m_sidesOfChoicesThatArriveAtBlocks.ensure(std::pair { &m_index, choosing.index() }, [&] {
        Walker::SidesThatArriveAtBlocks counted;
        forEachBlockWhereSideArrives([&](BlockID block) {
            ++counted.add(block.index(), 0).iterator->value;
        });
        return counted;
    }).iterator->value;
    return arrivals.get(m_block.id().index()) == numberOfWaysIn;
}

bool KnownThreading::terminatorMayBeWalkedByWaysApart()
{
    if (m_walker.isInLoop() && boundsProvenOnEdge(m_index, m_block.terminator(), 0))
        return false;
    for (BodyTarget target : m_block.terminator().targets()) {
        BlockID to = target.block();
        if (m_walker.inputBlock(m_frameIndex, to).head && m_index.isInLoop(m_block.id(), m_index.loopOf(to)))
            continue;
        if (to == m_block.id() || m_index.isLoopHead(to) || m_index.isEnteredByHandlerEdge(to) || m_index.block(to).isHandlerBlock() || m_walker.inputBlock(m_frameIndex, to).isEntered)
            return false;
    }
    return true;
}

ALWAYS_INLINE ValueID KnownThreading::valueFromInFront(const LookAhead& look, ValueID operand)
{
    while (operand >= look.bound && !m_index.isParameter(operand) && inputPassThroughOpcodes.contains(m_index.opcodeOf(operand)))
        operand = m_index.value(operand).operand(0);
    if (operand < look.bound)
        return operand;
    for (auto [parameter, argument] : look.brought) {
        if (parameter == operand)
            return argument;
    }
    return { };
}

void KnownThreading::lookBehindEdge(LookAhead& look, const PlaceLookedAt& from, const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    for (const PlaceLookedAt& place : look.places) {
        if (place.frameIndex == from.frameIndex && place.block == target.block())
            return;
    }
    if (from.frameIndex == m_frameIndex) {
        BodyBlock to = m_index.block(target.block());
        uint32_t position = 0;
        for (ValueID argument : target.arguments()) {
            if (ValueID value = valueFromInFront(look, argument))
                look.brought.append({ to.parameter(position), value });
            ++position;
        }
        for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex) {
            std::optional<BodyEdgeResult> result = m_index.edgeResult(terminator, targetIndex, target, proofIndex);
            if (ValueID value = result ? valueFromInFront(look, result->subject) : ValueID())
                look.brought.append({ result->parameter, value });
        }
    }
    look.places.append(PlaceLookedAt { from.frameIndex, target.block(), 0, from.call, from.given });
}

ALWAYS_INLINE void KnownThreading::lookBehindReturnOfCopy(LookAhead& look, const PlaceLookedAt& from, const BodyTerminator& terminator)
{
    if (!from.frameIndex)
        return;
    if (m_walker.m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& walked) { return walked.frameOfTurn == from.frameIndex; }))
        return;
    const Walker::Frame& copy = m_walker.m_frames[from.frameIndex];
    const Walker::Frame& caller = m_walker.m_frames[from.frameIndex - 1];
    if (copy.returnsInPlaceOfUnit || !copy.isCopiedByLinkOfChain)
        return;
    ValueID given;
    if (ValueID returned = terminator.kind() == TerminatorKind::Return ? terminator.operand(0) : ValueID()) {
        if (from.frameIndex == m_frameIndex)
            given = valueFromInFront(look, returned);
        else {
            while (returned > from.call && !copy.index->isParameter(returned) && inputPassThroughOpcodes.contains(copy.index->opcodeOf(returned)))
                returned = copy.index->value(returned).operand(0);
            if (returned == from.call)
                given = from.given;
        }
    }
    uint32_t begin = copy.callCopied.index() - caller.index->block(caller.block).firstValue().index() + 1;
    look.places.append(PlaceLookedAt { from.frameIndex - 1, caller.block, begin, copy.callCopied, given });
}

bool KnownThreading::calleeAsksFirstWhatItIsHanded(const BodyValue& operation, uint32_t operandIndex, std::optional<bool>& copyOfCalleePays)
{
    if (operation.opcode() != Opcode::Call || operandIndex < numberOfNamedOperandsOf(Opcode::Call) || operation.as<Opcode::Call>().isNeverInlined())
        return false;
    FunctionID callee = m_walker.facts().linked().calleeAt(frame().function, operation.id());
    const BodyIndex* body = callee ? m_walker.facts().bodies().find(callee) : nullptr;
    if (!body)
        return false;
    const Function& function = m_walker.program().at(callee);
    BodyBlock entry = body->block(entryBlock);
    const uint32_t position = operandIndex - numberOfNamedOperandsOf(Opcode::Call);
    const uint32_t index = function.firstParameterIndex() + position;
    if (position >= function.numberOfPositionalParameters() || index >= entry.numberOfParameters())
        return false;
    ValueID parameter = entry.parameter(index);
    std::optional<BodyUser> first = body->firstUser(parameter);
    if (!first || first->kind != BodyUseKind::Operand || !pathSensitiveOperations.contains(body->opcodeOf(first->value)))
        return false;
    BodyValue asking = body->value(first->value);
    const uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(asking.opcode());
    bool asks = false;
    for (uint32_t named = 0; named < numberOfNamedOperands; ++named)
        asks |= asking.operand(named) == parameter && !asking.operandUses(named).containsOnly({ OperandUse::Retains, OperandUse::Returns });
    if (!asks)
        return false;
    if (!copyOfCalleePays)
        copyOfCalleePays = copyOfCalleePaysAt(m_walker, callee, operation.as<Opcode::Call>().argumentsOperands().size(), { frame().function, m_walker.isInLoop() });
    return *copyOfCalleePays;
}

template<typename Functor>
bool KnownThreading::looksAhead(LookAhead& look, const Functor& isLeanedOn)
{
    Walker& walker = m_walker;
    bool hasPassedJoin = false;
    for (uint32_t number = 0; number < look.places.size() && number < mostBlocksLookedThroughInKnownThreading; ++number) {
        PlaceLookedAt place = look.places[number];
        KnownThreading here(walker, place.frameIndex, place.block);
        hasPassedJoin |= number >= look.numberOfPlacesAsked && !place.begin && here.m_index.numberOfIncomingEdges(place.block) != 1;
        if (number >= look.numberOfPlacesAsked) {
            if (place.begin) {
                const Walker::Frame& frame = here.frame();
                if (frame.endOfStretch != here.m_block.numberOfValues() || frame.numberOfStretchesBeingWalked != 1 || !here.endOfBlockMayBeWalkedForEachWayFrom(place.begin))
                    continue;
            } else if ((here.m_index.numberOfIncomingEdges(place.block) != 1 && !here.isWhereAllSidesOfOneChoiceMeet()) || !here.mayBeWalkedApartFromClosedWays() || !here.terminatorMayBeWalkedByWaysApart())
                continue;
        }
        for (uint32_t position = place.begin; position < here.m_block.numberOfValues(); ++position) {
            BodyValue operation = here.m_index.value(ValueID(here.firstValue() + position));
            bool isInstanceOfInLoop = operation.opcode() == Opcode::InstanceOf && !hasPassedJoin && (walker.isInLoop() || walker.isCurrentFrameRecursive());
            bool isEquality = allEqualities.contains(operation.opcode());
            if (!isInstanceOfInLoop && !isEquality && !(hasPassedJoin ? numericAndRelationalOperators : pathSensitiveOperations).contains(operation.opcode()))
                continue;
            bool looks = false;
            std::optional<bool> copyOfCalleePays;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (looks || (operation.operandUses(operandIndex).containsOnly({ OperandUse::Retains, OperandUse::Returns }) && !here.calleeAsksFirstWhatItIsHanded(operation, operandIndex, copyOfCalleePays)))
                    return;
                if (place.frameIndex == m_frameIndex) {
                    if (ValueID value = valueFromInFront(look, operand))
                        looks = isLeanedOn(value, Reference());
                    return;
                }
                while (operand > place.call && !here.m_index.isParameter(operand) && inputPassThroughOpcodes.contains(here.m_index.opcodeOf(operand)))
                    operand = here.m_index.value(operand).operand(0);
                if (operand == place.call)
                    looks = place.given && isLeanedOn(place.given, Reference());
                else if (operand < place.call) {
                    Reference name = walker.referenceBoundIn(here.frame(), operand);
                    looks = name && isLeanedOn(ValueID(), name);
                }
            });
            if (looks && isEquality)
                looks = here.frame().loopDepthAtCall || here.m_index.loopDepthOf(place.block) || here.isFrameRecursive();
            if (looks)
                return true;
        }
        BodyTerminator end = here.m_block.terminator();
        if ((end.kind() == TerminatorKind::Switch || end.kind() == TerminatorKind::SwitchString) && place.frameIndex == m_frameIndex) {
            if (ValueID value = valueFromInFront(look, end.operand(0)); value && isLeanedOn(value, Reference()))
                return true;
        }
        if (end.kind() == TerminatorKind::Jump)
            lookBehindEdge(look, place, end, 0, end.targets()[0]);
        else if (end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall)
            lookBehindReturnOfCopy(look, place, end);
    }
    return false;
}

bool KnownThreading::isMentionedBehindBlock(ValueID value)
{
    while (!m_index.isUnused(value)) {
        std::optional<BodyUser> user = m_index.onlyUser(value);
        if (!user || !user->value || !m_block.holds(user->value))
            return true;
        if (!inputPassThroughOpcodes.contains(m_index.opcodeOf(user->value)))
            return false;
        value = user->value;
    }
    return false;
}

bool KnownThreading::armsWouldStayApartBehindEndOfBlock(uint32_t begin, uint32_t end, Reference value, const BitVector& placesThatTypedRunKnowsBetter)
{
    ValueID firstOfStretch(firstValue() + begin);
    ValueID endOfStretch(firstValue() + end);
    auto isLeanedOn = [&](ValueID own, Reference nameInCaller) {
        if (!own)
            return value && nameInCaller == value;
        if (own >= firstOfStretch)
            return own < endOfStretch && placesThatTypedRunKnowsBetter.get(own.index() - firstOfStretch.index());
        return value && m_walker.referenceOf(own) == value;
    };
    PlaceLookedAt here { m_frameIndex, m_block.id(), end, { }, { } };
    BodyTerminator terminator = m_block.terminator();
    uint32_t targetIndex = 0;
    for (BodyTarget target : terminator.targets()) {
        uint32_t indexOfTarget = targetIndex++;
        BlockID block = target.block();
        if ((m_index.numberOfIncomingEdges(block) != 1 && terminator.kind() != TerminatorKind::Jump) || m_walker.inputBlock(m_frameIndex, block).isEntered)
            continue;
        KnownThreading to(m_walker, m_frameIndex, block);
        if (!to.mayBeWalkedApartFromClosedWays() || !to.terminatorMayBeWalkedByWaysApart())
            continue;
        LookAhead look;
        look.bound = m_block.endOfValues();
        lookBehindEdge(look, here, terminator, indexOfTarget, target);
        look.numberOfPlacesAsked = 1;
        if (looksAhead(look, isLeanedOn))
            return true;
    }
    if (terminator.kind() == TerminatorKind::Return || terminator.kind() == TerminatorKind::ReturnCall) {
        LookAhead look;
        look.bound = m_block.endOfValues();
        lookBehindReturnOfCopy(look, here, terminator);
        return looksAhead(look, isLeanedOn);
    }
    return false;
}

uint32_t KnownThreading::numberOfNamesBroughtApart(std::span<const uint32_t> ways)
{
    Vector<uint32_t, 8> slots;
    for (size_t index = 1; index < ways.size(); ++index) {
        m_walker.m_openEdges[ways[index]].names.forEachSlotNamedOtherwiseBy(m_walker.m_openEdges[ways[index - 1]].names, [&](uint32_t slot) {
            if (m_walker.isNamedByWaysInto(frame(), frame().duplicatedValues->slots[slot].value, m_block.id()))
                slots.append(slot);
        });
    }
    std::ranges::sort(slots);
    return static_cast<uint32_t>(std::ranges::unique(slots).begin() - slots.begin());
}

bool KnownThreading::leansOnWhatWaysHaveProven(std::span<const uint32_t> passing, std::span<const uint32_t> closed)
{
    Walker& walker = m_walker;
    auto typeOnWay = [&](uint32_t place, ValueID value, Reference name) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        if (value && value >= m_block.firstParameter())
            name = open.arguments[value.index() - m_block.firstParameter().index()];
        else if (value) {
            name = walker.isNamedByWaysInto(frame(), value, m_block.id()) ? frame().duplicatedValues->nameOn(open.names, value) : Reference();
            if (!name)
                name = walker.referenceOf(value);
        }
        while (Reference checked = name ? open.state.checkedNameOf(name) : Reference())
            name = checked;
        return walker.ownTypeOf(name);
    };
    LookAhead look;
    look.bound = m_block.firstValue();
    look.places.append(PlaceLookedAt { m_frameIndex, m_block.id(), 0, { }, { } });
    look.numberOfPlacesAsked = 1;
    return looksAhead(look, [&](ValueID operand, Reference nameInCaller) {
        TypeID proven;
        for (uint32_t place : passing) {
            TypeID type = typeOnWay(place, operand, nameInCaller);
            if (!type)
                return false;
            proven = proven ? walker.joinTypes(proven, type) : type;
        }
        return std::ranges::any_of(closed, [&](uint32_t place) {
            TypeID type = typeOnWay(place, operand, nameInCaller);
            return type && !walker.types().isSubset(type, proven);
        });
    });
}

bool KnownThreading::letAnswersOfCopyWalkEndOfBlockApart(ValueID call)
{
    m_begin = call.index() - firstValue() + 1;
    if (m_index.holdsAnyOf(opcodesThatMakeArguments) || m_block.terminator().kind() != TerminatorKind::Branch)
        return false;
    if (frame().block != m_block.id() || frame().endOfStretch != m_block.numberOfValues() || frame().numberOfStretchesBeingWalked != 1 || m_walker.isTailDuplicatingBlock(m_frameIndex) || !endOfBlockMayBeWalkedForEachWayFrom(m_begin))
        return false;
    ValueID asked = m_block.terminator().operand(0);
    uint32_t numberOfQuestions = 0;
    while (asked && asked > call && (inputPassThroughOpcodes.contains(m_index.opcodeOf(asked)) || m_index.opcodeOf(asked) == Opcode::ToBoolean)) {
        numberOfQuestions += m_index.opcodeOf(asked) == Opcode::ToBoolean;
        asked = m_index.value(asked).operand(0);
    }
    if (asked != call || numberOfOperationsBuiltIn(m_begin, m_block.numberOfValues(), numberOfQuestions) != numberOfQuestions)
        return false;
    return letTerminatorBeWalkedForEachWay(m_block.firstValue());
}

bool KnownThreading::letReturnsOfCopyWalkEndOfBlockApart(ValueID call, std::span<ReturnOfCopy* const> passing, std::span<ReturnOfCopy* const> closed)
{
    Walker& walker = m_walker;
    m_begin = call.index() - firstValue() + 1;
    if (m_index.holdsAnyOf(opcodesThatMakeArguments))
        return false;
    if (frame().block != m_block.id() || frame().endOfStretch != m_block.numberOfValues() || frame().numberOfStretchesBeingWalked != 1 || walker.isTailDuplicatingBlock(m_frameIndex) || !endOfBlockMayBeWalkedForEachWayFrom(m_begin))
        return false;
    auto typeOnWay = [&](const ReturnOfCopy& way, ValueID value, Reference name) {
        if (value)
            name = value == call ? way.value : walker.referenceOf(value);
        while (Reference checked = name ? way.state.checkedNameOf(name) : Reference())
            name = checked;
        return walker.ownTypeOf(name);
    };
    LookAhead look;
    look.bound = ValueID(firstValue() + m_begin);
    look.places.append(PlaceLookedAt { m_frameIndex, m_block.id(), m_begin, { }, { } });
    look.numberOfPlacesAsked = 1;
    bool leans = looksAhead(look, [&](ValueID operand, Reference nameInCaller) {
        TypeID proven;
        for (const ReturnOfCopy* way : passing) {
            TypeID type = typeOnWay(*way, operand, nameInCaller);
            if (!type)
                return false;
            proven = proven ? walker.joinTypes(proven, type) : type;
        }
        return std::ranges::any_of(closed, [&](const ReturnOfCopy* way) {
            TypeID type = typeOnWay(*way, operand, nameInCaller);
            return type && !walker.types().isSubset(type, proven);
        });
    });
    return leans && letTerminatorBeWalkedForEachWay(m_block.firstValue());
}

const Walker::OpenLoop* KnownThreading::loopWhoseQuietWaysGoToHead()
{
    LoopID inputLoop = m_index.loopOf(m_block.id());
    uint32_t placeOfLoop = inputLoop && !frame().placesOfOpenLoops.isEmpty() ? frame().placesOfOpenLoops[inputLoop.index()] : 0;
    if (!placeOfLoop)
        return nullptr;
    const Walker::OpenLoop& loop = m_walker.m_openLoops[placeOfLoop - 1];
    return loop.firstReloadBlock || loop.watchesTurns ? &loop : nullptr;
}

bool KnownThreading::letQuietReturnsOfCopyWalkEndOfBlockApart(ValueID call)
{
    const Walker::OpenLoop* loop = loopWhoseQuietWaysGoToHead();
    if (!loop || m_index.holdsAnyOf(opcodesThatMakeArguments))
        return false;
    m_begin = call.index() - firstValue() + 1;
    if (frame().block != m_block.id() || frame().endOfStretch != m_block.numberOfValues() || frame().numberOfStretchesBeingWalked != 1 || m_walker.isTailDuplicatingBlock(m_frameIndex) || !endOfBlockMayBeWalkedForEachWayFrom(m_begin))
        return false;
    bool oneSideIsLastBlockOfTurn = false;
    for (BodyTarget target : m_block.terminator().targets()) {
        BodyTerminator end = m_index.terminator(target.block());
        oneSideIsLastBlockOfTurn |= end.kind() == TerminatorKind::Jump && m_walker.inputBlock(m_frameIndex, end.targets()[0].block()).head == loop->head;
    }
    return oneSideIsLastBlockOfTurn && letTerminatorBeWalkedForEachWay(m_block.firstValue());
}

ALWAYS_INLINE bool KnownThreading::walkApartFromClosedWays()
{
    Walker& walker = m_walker;
    BlockID block = m_block.id();
    const WaysIn& ways = waysIn();
    if (!ways.allMayBeLed || !ways.numberLedByClosedArms || ways.numberLedByClosedArms == ways.places.size() || walker.isTailDuplicatingBlock(m_frameIndex) || !mayBeWalkedApartFromClosedWays())
        return false;
    Vector<uint32_t, 4> passing;
    Vector<uint32_t, 4> closed;
    for (uint32_t place : ways.places)
        (walker.m_openEdges[place].isLedByClosedArm ? closed : passing).append(place);
    if (numberOfOperationsBuiltIn(0, m_block.numberOfValues(), mostOperationsOfJoinWalkedForEachWay) + numberOfNamesBroughtApart(closed.span()) > mostOperationsOfJoinWalkedForEachWay)
        return false;
    passing.reverse();
    closed.reverse();
    if (!leansOnWhatWaysHaveProven(passing.span(), closed.span()))
        return false;
    if (!terminatorMayBeWalkedByWaysApart() || !letTerminatorBeWalkedForEachWay(m_block.firstParameter()))
        return false;

    uint32_t namesAtHead = walker.inputBlock(m_frameIndex, block).firstNarrowerName;
    bool standsInLoop = frame().loopDepthAtCall || m_index.loopDepthOf(block);
    bool closedWaysKeepSites = standsInLoop && std::ranges::all_of(closed, [&](uint32_t place) {
        return walker.m_openEdges[place].isLedByClosedArmThatKeepsSites;
    });
    uint32_t firstBlockOfClosedWays = walker.m_code.numberOfBlocks();
    {
        Walker::ClosedArm closedArm(walker);
        SetForScope waysOutKeepSites(walker.m_closedArmKeepsSites, closedWaysKeepSites);
        ++walker.m_coldCopyDepth;
        walkBlockOnWays(closed.span(), namesAtHead);
        --walker.m_coldCopyDepth;
    }
    for (uint32_t number = firstBlockOfClosedWays; closedWaysKeepSites && number < walker.m_code.numberOfBlocks(); ++number)
        walker.m_code.markRareBlockThatKeepsSites(BlockID(number));
    if (closedWaysKeepSites)
        walker.count(StatisticID::FallbackBlocksKeepingSiteCaches, walker.m_code.numberOfBlocks() - firstBlockOfClosedWays);
    walkBlockOnWays(passing.span(), namesAtHead);

    frame().blocksBeingTailDuplicated.shrink(0);
    walker.inputBlock(m_frameIndex, block).firstNarrowerName = namesAtHead;
    forgetValuesFrom(m_block.firstParameter());
    if (frame().duplicatedValues)
        walker.forgetNamesOfWay(frame());
    walker.m_block = { };
    walker.m_state.clear();
    return true;
}

bool KnownThreading::walkApartFromWaysThatDidSomething()
{
    Walker& walker = m_walker;
    BlockID block = m_block.id();
    LoopID inputLoop = m_index.loopOf(block);
    uint32_t placeOfLoop = inputLoop && !frame().placesOfOpenLoops.isEmpty() ? frame().placesOfOpenLoops[inputLoop.index()] : 0;
    if (!placeOfLoop || (!walker.m_openLoops[placeOfLoop - 1].firstReloadBlock && !walker.m_openLoops[placeOfLoop - 1].watchesTurns) || m_block.terminator().kind() != TerminatorKind::Jump)
        return false;
    const Walker::OpenLoop& loop = walker.m_openLoops[placeOfLoop - 1];
    for (BodyTarget target : m_block.terminator().targets()) {
        if (walker.inputBlock(m_frameIndex, target.block()).head != loop.head)
            return false;
    }

    Vector<uint32_t, 4> quiet;
    Vector<uint32_t, 4> others;
    for (uint32_t place = walker.inputBlock(m_frameIndex, block).firstOpenEdge; place != Walker::noOpenEdge; place = walker.m_openEdges[place].next) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        if (open.block || open.arguments.size() != m_block.numberOfParameters())
            return false;
        (open.state.hasOnlyKnownEffectsSince(loop.restartPoint) ? quiet : others).append(place);
    }
    if (quiet.isEmpty() || others.isEmpty() || walker.isTailDuplicatingBlock(m_frameIndex) || !mayBeWalkedApartFromClosedWays())
        return false;
    if (numberOfOperationsBuiltIn(0, m_block.numberOfValues(), mostOperationsOfJoinWalkedForEachWay) + numberOfNamesBroughtApart(quiet.span()) + numberOfNamesBroughtApart(others.span()) > mostOperationsOfJoinWalkedForEachWay)
        return false;
    quiet.reverse();
    others.reverse();
    if (!terminatorMayBeWalkedByWaysApart())
        return false;
    if (loop.watchesTurns) {
        walker.noteWhatTurnsOfLoopShow(loop, Walker::someWayRoundDidNothing);
        return false;
    }
    if (!letTerminatorBeWalkedForEachWay(m_block.firstParameter()))
        return false;

    uint32_t namesAtHead = walker.inputBlock(m_frameIndex, block).firstNarrowerName;
    walkBlockOnWays(others.span(), namesAtHead);
    walkBlockOnWays(quiet.span(), namesAtHead);

    frame().blocksBeingTailDuplicated.shrink(0);
    walker.inputBlock(m_frameIndex, block).firstNarrowerName = namesAtHead;
    forgetValuesFrom(m_block.firstParameter());
    if (frame().duplicatedValues)
        walker.forgetNamesOfWay(frame());
    walker.m_block = { };
    walker.m_state.clear();
    return true;
}

ALWAYS_INLINE bool KnownThreading::walkApartBySidesOfQuestionAskedBefore()
{
    Walker& walker = m_walker;
    BlockID block = m_block.id();
    BodyTerminator end = m_block.terminator();
    ValueID question = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!question || question != m_block.firstValue() || !m_block.numberOfValues() || m_index.opcodeOf(question) != Opcode::ToBoolean || !m_index.hasOneUse(question))
        return false;
    if (numberOfOperationsBuiltIn(0, m_block.numberOfValues(), 1) != 1)
        return false;
    ValueID operand = m_index.value(question).operand(OperandIndex::ToBoolean::value);
    if (!operand)
        return false;
    ValueID asked = m_index.stripCasts(operand);
    if (m_index.hasOneUse(operand) && m_index.hasOneUse(asked))
        return false;
    BlockID first = nearestBlockAboveThatBranchesOnTruthOf(m_index, asked, block);
    if (!first)
        return false;
    BlockID sides[2] = { m_index.terminator(first).targets()[1].block(), m_index.terminator(first).targets()[0].block() };
    if (sides[0] == sides[1])
        return false;
    auto comesFromSide = [&](BlockID left, bool answer) {
        BlockID side = sides[answer];
        if (side == block)
            return left == first;
        return m_index.numberOfIncomingEdges(side) == 1 && m_index.dominates(side, left);
    };

    Vector<uint32_t, 4> ways[3];
    for (uint32_t place = walker.inputBlock(m_frameIndex, block).firstOpenEdge; place != Walker::noOpenEdge; place = walker.m_openEdges[place].next) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        if (open.block || !open.inputBlockLeft || open.arguments.size() != m_block.numberOfParameters())
            return false;
        ways[comesFromSide(open.inputBlockLeft, true) ? 1 : comesFromSide(open.inputBlockLeft, false) ? 0 : 2].append(place);
    }
    if ((ways[0].isEmpty() && ways[1].isEmpty()) || walker.isTailDuplicatingBlock(m_frameIndex) || !mayBeWalkedApartFromClosedWays())
        return false;
    if (numberOfNamesBroughtApart(ways[0].span()) + numberOfNamesBroughtApart(ways[1].span()) + numberOfNamesBroughtApart(ways[2].span()) > mostOperationsOfJoinWalkedForEachWay)
        return false;
    if (!terminatorMayBeWalkedByWaysApart() || !letTerminatorBeWalkedForEachWay(m_block.firstParameter()))
        return false;

    uint32_t namesAtHead = walker.inputBlock(m_frameIndex, block).firstNarrowerName;
    for (unsigned answer = 0; answer < 3; ++answer) {
        ways[answer].reverse();
        SetForScope threadedConditionScope(walker.m_threadedCondition, answer < 2 ? question : ValueID());
        SetForScope threadedConditionValueScope(walker.m_threadedConditionValue, answer == 1);
        SetForScope threadedConditionFrameScope(walker.m_threadedConditionFrame, m_frameIndex);
        if (answer < 2 && !ways[answer].isEmpty()) {
            walker.bind(question, walker.booleanOfUnit(answer));
            frame().firstOperationToWalk = 1;
            ++walker.m_threadedConditionCount;
        }
        walkBlockOnWays(ways[answer].span(), namesAtHead);
        frame().firstOperationToWalk = 0;
    }

    frame().blocksBeingTailDuplicated.shrink(0);
    walker.inputBlock(m_frameIndex, block).firstNarrowerName = namesAtHead;
    forgetValuesFrom(m_block.firstParameter());
    if (frame().duplicatedValues)
        walker.forgetNamesOfWay(frame());
    walker.m_block = { };
    walker.m_state.clear();
    return true;
}

bool KnownThreading::takesApartParameter(ValueID parameter)
{
    for (uint32_t place = 0; place < m_block.numberOfValues(); ++place) {
        BodyValue operation = m_index.value(ValueID(firstValue() + place));
        auto names = [&](ValueID operand) {
            while (operand && operand != parameter && operand >= m_block.firstValue() && inputPassThroughOpcodes.contains(m_index.opcodeOf(operand)))
                operand = m_index.value(operand).operand(0);
            return operand == parameter;
        };
        switch (operation.opcode()) {
        case Opcode::PropGet:
            if (!operation.as<Opcode::PropGet>().isFreeName() && names(operation.as<Opcode::PropGet>().objectOperand()))
                return true;
            break;
        case Opcode::CopyDataProperties:
            if (names(operation.as<Opcode::CopyDataProperties>().sourceOperand()))
                return true;
            break;
        case Opcode::Call: {
            auto call = operation.as<Opcode::Call>();
            ValueID callee = call.calleeOperand();
            if (!callee || m_index.isParameter(callee) || m_index.opcodeOf(callee) != Opcode::Builtin || m_walker.program().builtins().canonicalID(m_index.value(callee).as<Opcode::Builtin>().builtin()) != Builtins::ObjectAssign)
                break;
            if (std::ranges::any_of(call.argumentsOperands(), names))
                return true;
            break;
        }
        default:
            break;
        }
    }
    return false;
}

bool KnownThreading::callsParameter(ValueID parameter, uint32_t numberOfBlocksLookedThrough)
{
    auto names = [&](ValueID operand) {
        while (operand && operand != parameter && operand >= m_block.firstValue() && inputPassThroughOpcodes.contains(m_index.opcodeOf(operand)))
            operand = m_index.value(operand).operand(0);
        return operand == parameter;
    };
    for (uint32_t place = 0; place < m_block.numberOfValues(); ++place) {
        BodyValue operation = m_index.value(ValueID(firstValue() + place));
        if (operation.opcode() == Opcode::Construct && !operation.as<Opcode::Construct>().newTargetOperand() && names(operation.as<Opcode::Construct>().calleeOperand()))
            return true;
        if (operation.opcode() != Opcode::Call)
            continue;
        auto call = operation.as<Opcode::Call>();
        if (names(call.calleeOperand()))
            return true;
        FunctionID helper = m_walker.facts().linked().calleeAt(frame().function, operation.id());
        const BodyIndex* body = helper ? m_walker.facts().bodies().find(helper) : nullptr;
        if (!body || m_walker.facts().summaries().at(helper).has(SummaryTrait::HasLoops))
            continue;
        const Function& function = m_walker.program().at(helper);
        BodyBlock entry = body->block(entryBlock);
        std::span<const ValueID> written = call.argumentsOperands();
        for (unsigned index = function.firstParameterIndex(); index < function.numberOfEntryParameters() && index < entry.numberOfParameters() && index - function.firstParameterIndex() < written.size(); ++index) {
            if (function.entryParameterKind(index) != EntryParameter::Parameter || !names(written[index - function.firstParameterIndex()]))
                continue;
            ValueID handed = entry.parameter(index);
            for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
                for (BodyValue inHelper : body->block(BlockID(number)).values()) {
                    if ((inHelper.opcode() == Opcode::Call || inHelper.opcode() == Opcode::Construct) && body->stripCasts(inHelper.calleeOperand()) == handed)
                        return true;
                }
            }
        }
    }
    BodyTerminator end = m_block.terminator();
    if (end.kind() != TerminatorKind::Jump || numberOfBlocksLookedThrough == mostBlocksLookedThroughInKnownThreading)
        return false;
    BodyTarget target = end.targets()[0];
    if (m_index.isLoopHead(target.block()) || target.block() == m_block.id())
        return false;
    uint32_t position = 0;
    for (ValueID argument : target.arguments()) {
        if (names(argument) && KnownThreading(m_walker, m_frameIndex, target.block()).callsParameter(m_index.block(target.block()).parameter(position), numberOfBlocksLookedThrough + 1))
            return true;
        ++position;
    }
    return false;
}

bool KnownThreading::takesParameterAsKey(ValueID parameter)
{
    for (uint32_t place = 0; place < m_block.numberOfValues(); ++place) {
        BodyValue operation = m_index.value(ValueID(firstValue() + place));
        if (!accessesUnderComputedKeys.contains(operation.opcode()))
            continue;
        ValueID key = operation.propertyOperand();
        while (key && key != parameter && key >= m_block.firstValue() && inputPassThroughOpcodes.contains(m_index.opcodeOf(key)))
            key = m_index.value(key).operand(0);
        if (key == parameter)
            return true;
    }
    return false;
}

bool KnownThreading::walkForEachWayThatBringsObjectInSight()
{
    Walker& walker = m_walker;
    BlockID block = m_block.id();
    if (!m_block.numberOfParameters() || !waysIn().allMayBeLed || waysIn().numberLedByClosedArms || waysIn().places.size() < 2 || walker.isTailDuplicatingBlock(m_frameIndex))
        return false;
    Vector<uint32_t, 4> ways = waysIn().places;

    auto bringsObjectInSight = [&](uint32_t place, uint32_t parameter) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        Reference brought = open.arguments[parameter];
        if (!brought.isValue())
            return false;
        ValueID cell = underlyingCellOf(walker.code(), brought.id());
        return !walker.code().isConstant(cell) && open.state.isFresh(Reference::value(cell)) && open.state.orderWordOf(Reference::value(cell));
    };
    auto calleeBroughtBy = [&](uint32_t place, uint32_t parameter) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        Reference brought = open.arguments[parameter];
        while (Reference checked = brought ? open.state.checkedNameOf(brought) : Reference())
            brought = checked;
        TypeID own = walker.ownTypeOf(brought);
        if (!own)
            return FunctionID();
        Type type = walker.types().at(own);
        return type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    };
    auto answerBroughtBy = [&](uint32_t place, uint32_t parameter) -> std::optional<bool> {
        TypeID own = walker.ownTypeOf(walker.m_openEdges[place].arguments[parameter]);
        if (own == TypeArena::fromPrimitives(Primitive::True))
            return true;
        if (own == TypeArena::fromPrimitives(Primitive::False))
            return false;
        return std::nullopt;
    };
    bool pays = false;
    bool waysBringCallees = false;
    bool waysBringTextsOfKey = false;
    bool waysBringAnswers = false;
    if (TerminatorKind end = m_block.terminator().kind(); !numberOfOperationsBuiltIn(0, m_block.numberOfValues(), 0) && (end == TerminatorKind::Branch || (end == TerminatorKind::Return && m_frameIndex && !frame().returnsInPlaceOfUnit))) {
        ValueID asked = m_block.terminator().operand(0);
        while (asked && asked >= m_block.firstValue() && inputPassThroughOpcodes.contains(m_index.opcodeOf(asked)))
            asked = m_index.value(asked).operand(0);
        if (asked && asked >= m_block.firstParameter() && asked < m_block.firstValue()) {
            uint32_t parameter = asked.index() - m_block.firstParameter().index();
            bool someSaysYes = false;
            bool someSaysNo = false;
            bool everyWayAnswers = std::ranges::all_of(ways, [&](uint32_t place) {
                std::optional<bool> answer = answerBroughtBy(place, parameter);
                someSaysYes |= answer && *answer;
                someSaysNo |= answer && !*answer;
                return !!answer;
            });
            waysBringAnswers = everyWayAnswers && someSaysYes && someSaysNo;
            pays = waysBringAnswers;
        }
    }
    for (uint32_t parameter = 0; parameter < m_block.numberOfParameters() && !pays; ++parameter) {
        bool everyWayBringsOne = std::ranges::all_of(ways, [&](uint32_t place) {
            return bringsObjectInSight(place, parameter);
        });
        pays = everyWayBringsOne && takesApartParameter(m_block.parameter(parameter));
        if (!pays && walker.isInLoop()) {
            waysBringTextsOfKey = takesParameterAsKey(m_block.parameter(parameter)) && std::ranges::all_of(ways, [&](uint32_t place) {
                return !!nameThatKeyIs(walker, walker.m_openEdges[place].arguments[parameter]);
            });
            pays = waysBringTextsOfKey;
        }
        if (pays || !walker.has(Mechanism::DirectCalls) || !walker.isInLoop())
            continue;
        FunctionID first = calleeBroughtBy(ways[0], parameter);
        bool everyWayBringsOneCallee = !!first;
        bool calleesDiffer = false;
        for (size_t way = 1; way < ways.size() && everyWayBringsOneCallee; ++way) {
            FunctionID callee = calleeBroughtBy(ways[way], parameter);
            everyWayBringsOneCallee = !!callee;
            calleesDiffer |= callee != first;
        }
        waysBringCallees = everyWayBringsOneCallee && calleesDiffer && callsParameter(m_block.parameter(parameter));
        pays = waysBringCallees;
    }
    if (!pays || !mayBeWalkedApartFromClosedWays())
        return false;
    if (numberOfOperationsBuiltIn(0, m_block.numberOfValues(), mostOperationsOfJoinWalkedForEachWay) * (ways.size() - 1) > mostOperationsOfJoinWalkedForEachWay)
        return false;
    if (!terminatorMayBeWalkedByWaysApart() || !letTerminatorBeWalkedForEachWay(m_block.firstParameter()))
        return false;

    ways.reverse();
    ++walker.m_duplicatedJoinCount;
    if (waysBringCallees)
        walker.count(StatisticID::JoinsDuplicatedPerCallee);
    if (waysBringTextsOfKey)
        walker.count(StatisticID::JoinsDuplicatedPerKeyString);
    uint32_t namesAtHead = walker.inputBlock(m_frameIndex, block).firstNarrowerName;
    for (uint32_t place : ways)
        walkBlockOnWays(singleElementSpan(place), namesAtHead);

    frame().blocksBeingTailDuplicated.shrink(0);
    walker.inputBlock(m_frameIndex, block).firstNarrowerName = namesAtHead;
    forgetValuesFrom(m_block.firstParameter());
    if (frame().duplicatedValues)
        walker.forgetNamesOfWay(frame());
    walker.m_block = { };
    walker.m_state.clear();
    return true;
}

bool Walker::isTailDuplicatingBlock(size_t frameIndex) const
{
    const Frame& frame = m_frames[frameIndex];
    return frame.blocksBeingTailDuplicated.contains(frame.block);
}

bool Walker::inlineCalleeAsLinkOfChainOfItsOpcodes(const BodyValue& call)
{
    SetForScope callCopiedByLinkOfChain(m_callCopiedByLinkOfChain, call.id());
    SetForScope numberOfFramesAtThatCall(m_numberOfFramesAtCallCopiedByLinkOfChain, m_frames.size());
    return inlineCallee(*this, call);
}

bool Walker::armsMayTakeBranchOf(BlockID inputBlock, uint32_t end, Reference value) const
{
    const Frame& frame = m_frames.last();
    BodyBlock block = frame.index->block(inputBlock);
    TerminatorKind kind = block.terminator().kind();
    bool returnsFromCopy = (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall) && !walksOwnBodyOfUnit() && !frame.returnsInPlaceOfUnit;
    bool mayBeTaken = goesByOneOperand(kind) || kind == TerminatorKind::Jump || returnsFromCopy;
    if ((value && !value.isValue()) || !mayBeTaken || frame.endOfStretch != block.numberOfValues() || block.numberOfValues() - end > mostOperationsOfJoinWalkedForEachWay)
        return false;
    return !hasStringsInBuilders() && !standsInForValueOfFrame();
}

bool Walker::armsTakeBranchForProofOf(BlockID inputBlock, uint32_t begin, uint32_t end, Reference value, const BitVector& placesThatTypedRunKnowsBetter)
{
    if (!armsMayTakeBranchOf(inputBlock, end, value))
        return false;
    BodyBlock block = m_frames.last().index->block(inputBlock);
    if (!goesByOneOperand(block.terminator().kind()))
        value = { };
    if (!value) {
        KnownThreading threading(*this, inputBlock);
        bool isMentionedBehind = false;
        for (size_t place : placesThatTypedRunKnowsBetter)
            isMentionedBehind = isMentionedBehind || threading.isMentionedBehindBlock(ValueID(static_cast<uint32_t>(block.firstValue().index() + begin + place)));
        if (!isMentionedBehind)
            return false;
    }
    return KnownThreading(*this, inputBlock).armsWouldStayApartBehindEndOfBlock(begin, end, value, placesThatTypedRunKnowsBetter);
}

bool Walker::letReturnsOfCopyWalkEndOfBlockApart(ValueID call, std::span<Frame::ReturnOfCopy* const> passing, std::span<Frame::ReturnOfCopy* const> closed)
{
    if (m_hasFailed || !has(Mechanism::Guesses) || !m_opensGuesses || m_coldCopyDepth || hasStringsInBuilders() || standsInForValueOfFrame())
        return false;
    return KnownThreading(*this, m_frames.last().block).letReturnsOfCopyWalkEndOfBlockApart(call, passing, closed);
}

std::optional<uint32_t> Walker::turnWhoseQuietWaysGoToHeadAround(BlockID inputBlock)
{
    if (m_hasFailed || m_openLoops.isEmpty() || m_coldCopyDepth || hasStringsInBuilders() || standsInForValueOfFrame())
        return std::nullopt;
    const OpenLoop* loop = KnownThreading(*this, inputBlock).loopWhoseQuietWaysGoToHead();
    return loop ? std::optional(loop->restartPoint) : std::nullopt;
}

bool Walker::letAnswersOfCopyWalkEndOfBlockApart(ValueID call)
{
    if (m_hasFailed || !has(Mechanism::Guesses) || m_coldCopyDepth || hasStringsInBuilders() || standsInForValueOfFrame())
        return false;
    return KnownThreading(*this, m_frames.last().block).letAnswersOfCopyWalkEndOfBlockApart(call);
}

bool Walker::letQuietReturnsOfCopyWalkEndOfBlockApart(ValueID call)
{
    return KnownThreading(*this, m_frames.last().block).letQuietReturnsOfCopyWalkEndOfBlockApart(call);
}

void Walker::walkEndOfBlockBehindReturnsOfCopy(ValueID call)
{
    storeValueBehindItsDefinition(call);
    Frame& frame = m_frames.last();
    frame.walkedUpTo = call.index() - frame.index->block(frame.block).firstValue().index() + 1;
    if (!isUnreachable() && !threadBranchOf(frame.block))
        fail("what is left of a block behind a copy is not walked by the ways that were to walk it apart"_s);
}

bool Walker::walkBlockApart(BlockID inputBlock)
{
    if (m_hasFailed || m_coldCopyDepth || hasStringsInBuilders())
        return false;
    KnownThreading threading(*this, inputBlock);
    bool opensGuesses = has(Mechanism::Guesses) && m_opensGuesses;
    if (opensGuesses && threading.walkApartFromClosedWays())
        return true;
    if (!m_openLoops.isEmpty() && !m_hasFailed && threading.walkApartFromWaysThatDidSomething())
        return true;
    if (m_opensGuesses && !m_hasFailed && has(Mechanism::EscapeAnalysis) && has(Mechanism::TypedOperations) && threading.walkForEachWayThatBringsObjectInSight())
        return true;
    return opensGuesses && !m_hasFailed && has(Mechanism::Folding) && threading.walkApartBySidesOfQuestionAskedBefore();
}

bool Walker::threadBranchOf(BlockID inputBlock)
{
    if (isUnreachable() || m_hasFailed)
        return false;
    return KnownThreading(*this, inputBlock).endArmWithBranch();
}

bool Walker::canThreadBranchAfter(const BodyValue& operation)
{
    const Frame& frame = m_frames.last();
    BodyBlock block = frame.index->block(frame.block);
    if (isUnreachable() || m_hasFailed || !block.holds(operation.id()) || hasStringsInBuilders())
        return false;
    return KnownThreading(*this, frame.block).armsMayWalkEndOfBlockFrom(operation.id().index() - block.firstValue().index() + 1);
}

bool Walker::canThreadBranchFrom(ValueID first)
{
    const Frame& frame = m_frames.last();
    BodyBlock block = frame.index->block(frame.block);
    if (isUnreachable() || m_hasFailed || !block.holds(first) || hasStringsInBuilders())
        return false;
    return KnownThreading(*this, frame.block).armsMayWalkEndOfBlockFrom(first.index() - block.firstValue().index());
}

bool Walker::threadBranchFrom(ValueID first)
{
    Frame& frame = m_frames.last();
    frame.walkedUpTo = first.index() - frame.index->block(frame.block).firstValue().index();
    return threadBranchOf(frame.block);
}

bool Walker::threadBranchAfter(const BodyValue& operation)
{
    storeValueBehindItsDefinition(operation.id());
    Frame& frame = m_frames.last();
    frame.walkedUpTo = operation.id().index() - frame.index->block(frame.block).firstValue().index() + 1;
    return threadBranchOf(frame.block);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
