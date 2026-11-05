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

static constexpr OpcodeSet operationsTypedByWayInKnownThreading = allRelations | allBitwiseOperators
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
    bool armsWouldStayApartBehindEndOfBlock(uint32_t begin, uint32_t end, Reference value, const BitVector& placesThatTypedRunKnowsBetter);
    bool isMentionedBehindBlock(ValueID);
    bool letReturnsOfCopyWalkEndOfBlockApart(ValueID call, std::span<ReturnOfCopy* const> passing, std::span<ReturnOfCopy* const> closed);

private:
    Walker::Frame& frame() { return m_walker.m_frames[m_frameIndex]; }
    uint32_t firstValue() const { return m_block.firstValue().index(); }
    bool operationsMayBeWalkedAgain(uint32_t begin, uint32_t end);
    bool terminatorMayBeWalkedAgain();
    bool letTerminatorBeWalkedForEachWay(ValueID firstBuiltOnEachWay);
    bool mayBeWalkedApartFromClosedWays();
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
    bool endOfBlockMayBeWalkedForEachWayFrom(uint32_t begin);
    uint32_t numberOfNamesBroughtApart(std::span<const uint32_t> ways);
    bool leansOnWhatWaysHaveProven(std::span<const uint32_t> passing, std::span<const uint32_t> closed);
    bool takesApartParameter(ValueID parameter);
    bool letArmsWalkEndOfBlockFrom(uint32_t begin);
    void forgetValuesThatNobodyReadsAnyMore();
    void forgetValuesFrom(ValueID first);
    bool isReadBehindBlock(ValueID);

    uint32_t numberOfOperationsBuiltIn(uint32_t begin, uint32_t end);

    void walkBlockOnWays(std::span<const uint32_t> places, uint32_t namesAtHead);

    Walker& m_walker;
    size_t m_frameIndex;
    const BodyIndex& m_index;
    BodyBlock m_block;
    uint32_t m_begin { 0 };
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

void KnownThreading::forgetValuesThatNobodyReadsAnyMore()
{
    if (!frame().loopsBeingBuiltTwice.isEmpty() || !frame().valuesBuiltTwice || m_walker.m_numberOfMarksOfTries)
        return;
    uint32_t here = m_block.firstParameter().index();
    Walker::Frame::ValuesBuiltTwice& listed = *frame().valuesBuiltTwice;
    for (uint32_t slot : listed.slotsListedAnew) {
        if (listed.slots[slot].value.index() >= frame().index->numberOfValues())
            continue;
        listed.slotsByLastUse.append({ m_walker.m_liveness.lastUse(listed.slots[slot].value).index(), slot });
        std::ranges::push_heap(listed.slotsByLastUse, std::ranges::greater());
    }
    listed.slotsListedAnew.shrink(0);
    while (!listed.slotsByLastUse.isEmpty() && listed.slotsByLastUse.first().first < here) {
        std::ranges::pop_heap(listed.slotsByLastUse, std::ranges::greater());
        Walker::Frame::ValuesBuiltTwice::Slot& dead = listed.slots[listed.slotsByLastUse.takeLast().second];
        if (std::exchange(dead.ordinal, 0))
            frame().references[dead.value.index()] = Reference();
    }
}

void KnownThreading::forgetValuesFrom(ValueID first)
{
    for (uint32_t value = first.index(); value < m_block.endOfValues().index(); ++value)
        frame().references[value] = Reference();
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
    if (frame().blocksWalkedForEachWay.contains(m_block.id()))
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
    if (frame().valuesBuiltTwice) {
        for (const auto& given : frame().valuesBuiltTwice->namesGivenForGood) {
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
    if (frame().valuesBuiltTwice) {
        frame().valuesBuiltTwice->namesGivenForGood.removeAllMatching([&](const auto& given) {
            return given.block == m_block.id();
        });
    }
    for (ValueID value : wouldBeNamedForGood)
        frame().valuesBuiltTwice->remove(value);
    for (ValueID value : readBehind)
        frame().listAsBuiltTwice(value);
    frame().blocksWalkedForEachWay.shrink(0);
    frame().blocksWalkedForEachWay.append(m_block.id());
    return true;
}

bool KnownThreading::letArmsWalkEndOfBlockFrom(uint32_t begin)
{
    return endOfBlockMayBeWalkedForEachWayFrom(begin) && letTerminatorBeWalkedForEachWay(m_block.firstValue());
}

bool KnownThreading::endOfBlockMayBeWalkedForEachWayFrom(uint32_t begin)
{
    uint32_t numberOfOperations = m_block.numberOfValues();
    if (begin > numberOfOperations || numberOfOperations - begin > mostOperationsOfJoinWalkedForEachWay)
        return false;
    TerminatorKind end = m_block.terminator().kind();
    bool returnsFromCopy = (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall) && m_frameIndex && !frame().returnsInPlaceOfUnit;
    bool mayBeTaken = goesByOneOperand(end) || end == TerminatorKind::Jump || returnsFromCopy;
    if (!mayBeTaken || m_block.hasHandler() || m_index.hasIrreducibleFlow())
        return false;
    if (m_index.holdsParametersReadByReleaseHintsAlone() || m_index.opcodes().contains(Opcode::ReleaseHint))
        return false;
    return operationsMayBeWalkedAgain(begin, numberOfOperations) && terminatorMayBeWalkedAgain();
}

uint32_t KnownThreading::numberOfOperationsBuiltIn(uint32_t begin, uint32_t end)
{
    uint32_t number = 0;
    for (uint32_t place = begin; place < end; ++place)
        number += !operationsThatBuildNothingInKnownThreading.contains(m_index.opcodeOf(ValueID(firstValue() + place)));
    return number;
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
    return frame().block == m_block.id() && frame().endOfStretch == m_block.numberOfValues() && letArmsWalkEndOfBlockFrom(begin);
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
    if (m_index.holdsParametersReadByReleaseHintsAlone() || m_index.opcodes().contains(Opcode::ReleaseHint))
        return false;
    TerminatorKind end = m_block.terminator().kind();
    if (end != TerminatorKind::Return && end != TerminatorKind::ReturnCall && !goesByOneOperand(end) && end != TerminatorKind::Jump)
        return false;
    uint32_t numberOfOperations = m_block.numberOfValues();
    return numberOfOperationsBuiltIn(0, numberOfOperations) <= mostOperationsOfJoinWalkedForEachWay && operationsMayBeWalkedAgain(0, numberOfOperations);
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

ValueID KnownThreading::valueFromInFront(const LookAhead& look, ValueID operand)
{
    while (operand >= look.bound && !m_index.isParameter(operand) && inputOperationsThatHandOn.contains(m_index.opcodeOf(operand)))
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

void KnownThreading::lookBehindReturnOfCopy(LookAhead& look, const PlaceLookedAt& from, const BodyTerminator& terminator)
{
    if (!from.frameIndex)
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
            while (returned > from.call && !copy.index->isParameter(returned) && inputOperationsThatHandOn.contains(copy.index->opcodeOf(returned)))
                returned = copy.index->value(returned).operand(0);
            if (returned == from.call)
                given = from.given;
        }
    }
    uint32_t begin = copy.callCopied.index() - caller.index->block(caller.block).firstValue().index() + 1;
    look.places.append(PlaceLookedAt { from.frameIndex - 1, caller.block, begin, copy.callCopied, given });
}

template<typename Functor>
bool KnownThreading::looksAhead(LookAhead& look, const Functor& isLeanedOn)
{
    Walker& walker = m_walker;
    for (uint32_t number = 0; number < look.places.size() && number < mostBlocksLookedThroughInKnownThreading; ++number) {
        PlaceLookedAt place = look.places[number];
        KnownThreading here(walker, place.frameIndex, place.block);
        if (number >= look.numberOfPlacesAsked) {
            if (place.begin) {
                const Walker::Frame& frame = here.frame();
                if (frame.endOfStretch != here.m_block.numberOfValues() || frame.numberOfStretchesBeingWalked != 1 || !here.endOfBlockMayBeWalkedForEachWayFrom(place.begin))
                    continue;
            } else if (here.m_index.numberOfIncomingEdges(place.block) != 1 || !here.mayBeWalkedApartFromClosedWays() || !here.terminatorMayBeWalkedByWaysApart())
                continue;
        }
        for (uint32_t position = place.begin; position < here.m_block.numberOfValues(); ++position) {
            BodyValue operation = here.m_index.value(ValueID(here.firstValue() + position));
            if (!operationsTypedByWayInKnownThreading.contains(operation.opcode()))
                continue;
            bool looks = false;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (looks || operation.operandUses(operandIndex).containsOnly({ OperandUse::Retains, OperandUse::Returns }))
                    return;
                if (place.frameIndex == m_frameIndex) {
                    if (ValueID value = valueFromInFront(look, operand))
                        looks = isLeanedOn(value, Reference());
                    return;
                }
                while (operand > place.call && !here.m_index.isParameter(operand) && inputOperationsThatHandOn.contains(here.m_index.opcodeOf(operand)))
                    operand = here.m_index.value(operand).operand(0);
                if (operand == place.call)
                    looks = place.given && isLeanedOn(place.given, Reference());
                else if (operand < place.call) {
                    Reference name = walker.referenceBoundIn(here.frame(), operand);
                    looks = name && isLeanedOn(ValueID(), name);
                }
            });
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
        if (!inputOperationsThatHandOn.contains(m_index.opcodeOf(user->value)))
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
            if (m_walker.isNamedByWaysInto(frame(), frame().valuesBuiltTwice->slots[slot].value, m_block.id()))
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
            name = walker.isNamedByWaysInto(frame(), value, m_block.id()) ? frame().valuesBuiltTwice->nameOn(open.names, value) : Reference();
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

bool KnownThreading::letReturnsOfCopyWalkEndOfBlockApart(ValueID call, std::span<ReturnOfCopy* const> passing, std::span<ReturnOfCopy* const> closed)
{
    Walker& walker = m_walker;
    m_begin = call.index() - firstValue() + 1;
    if (m_index.holdsAnyOf(opcodesThatMakeArguments))
        return false;
    if (frame().block != m_block.id() || frame().endOfStretch != m_block.numberOfValues() || frame().numberOfStretchesBeingWalked != 1 || walker.isWalkingBlockForEachWay(m_frameIndex) || !endOfBlockMayBeWalkedForEachWayFrom(m_begin))
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

bool KnownThreading::walkApartFromClosedWays()
{
    Walker& walker = m_walker;
    BlockID block = m_block.id();
    Vector<uint32_t, 4> passing;
    Vector<uint32_t, 4> closed;
    uint32_t firstWay = walker.inputBlock(m_frameIndex, block).firstOpenEdge;
    for (uint32_t place = firstWay; place != Walker::noOpenEdge; place = walker.m_openEdges[place].next) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        if (open.block || open.arguments.size() != m_block.numberOfParameters())
            return false;
        (open.isLedByClosedArm ? closed : passing).append(place);
    }
    if (passing.isEmpty() || closed.isEmpty() || walker.isWalkingBlockForEachWay(m_frameIndex) || !mayBeWalkedApartFromClosedWays())
        return false;
    if (numberOfOperationsBuiltIn(0, m_block.numberOfValues()) + numberOfNamesBroughtApart(closed.span()) > mostOperationsOfJoinWalkedForEachWay)
        return false;
    passing.reverse();
    closed.reverse();
    if (!leansOnWhatWaysHaveProven(passing.span(), closed.span()))
        return false;
    if (!terminatorMayBeWalkedByWaysApart() || !letTerminatorBeWalkedForEachWay(m_block.firstParameter()))
        return false;

    uint32_t namesAtHead = walker.inputBlock(m_frameIndex, block).firstNarrowerName;
    {
        Walker::ClosedArm closedArm(walker);
        ++walker.m_numberOfCopiesBuiltForSize;
        walkBlockOnWays(closed.span(), namesAtHead);
        --walker.m_numberOfCopiesBuiltForSize;
    }
    walkBlockOnWays(passing.span(), namesAtHead);

    frame().blocksWalkedForEachWay.shrink(0);
    walker.inputBlock(m_frameIndex, block).firstNarrowerName = namesAtHead;
    forgetValuesFrom(m_block.firstParameter());
    if (frame().valuesBuiltTwice)
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
    if (!placeOfLoop || (!walker.m_openLoops[placeOfLoop - 1].frontOfQuietTurns && !walker.m_openLoops[placeOfLoop - 1].watchesTurns) || m_block.terminator().kind() != TerminatorKind::Jump)
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
        (open.state.noOtherCodeRanInTurn(loop.numberOfTurn) ? quiet : others).append(place);
    }
    if (quiet.isEmpty() || others.isEmpty() || walker.isWalkingBlockForEachWay(m_frameIndex) || !mayBeWalkedApartFromClosedWays())
        return false;
    if (numberOfOperationsBuiltIn(0, m_block.numberOfValues()) + numberOfNamesBroughtApart(quiet.span()) + numberOfNamesBroughtApart(others.span()) > mostOperationsOfJoinWalkedForEachWay)
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

    frame().blocksWalkedForEachWay.shrink(0);
    walker.inputBlock(m_frameIndex, block).firstNarrowerName = namesAtHead;
    forgetValuesFrom(m_block.firstParameter());
    if (frame().valuesBuiltTwice)
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
            while (operand && operand != parameter && operand >= m_block.firstValue() && inputOperationsThatHandOn.contains(m_index.opcodeOf(operand)))
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

bool KnownThreading::walkForEachWayThatBringsObjectInSight()
{
    Walker& walker = m_walker;
    BlockID block = m_block.id();
    Vector<uint32_t, 4> ways;
    uint32_t firstWay = walker.inputBlock(m_frameIndex, block).firstOpenEdge;
    for (uint32_t place = firstWay; place != Walker::noOpenEdge; place = walker.m_openEdges[place].next) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        if (open.block || open.isLedByClosedArm || open.arguments.size() != m_block.numberOfParameters())
            return false;
        ways.append(place);
    }
    if (ways.size() < 2 || walker.isWalkingBlockForEachWay(m_frameIndex))
        return false;

    auto bringsObjectInSight = [&](uint32_t place, uint32_t parameter) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        Reference brought = open.arguments[parameter];
        if (!brought.isValue())
            return false;
        ValueID cell = rootOfCellInSweptCode(walker.code(), brought.id());
        return !walker.code().isConstant(cell) && open.state.isFresh(Reference::value(cell)) && open.state.orderWordOf(Reference::value(cell));
    };
    bool pays = false;
    for (uint32_t parameter = 0; parameter < m_block.numberOfParameters() && !pays; ++parameter) {
        bool everyWayBringsOne = std::ranges::all_of(ways, [&](uint32_t place) {
            return bringsObjectInSight(place, parameter);
        });
        pays = everyWayBringsOne && takesApartParameter(m_block.parameter(parameter));
    }
    if (!pays || !mayBeWalkedApartFromClosedWays())
        return false;
    if (numberOfOperationsBuiltIn(0, m_block.numberOfValues()) * (ways.size() - 1) > mostOperationsOfJoinWalkedForEachWay)
        return false;
    if (!terminatorMayBeWalkedByWaysApart() || !letTerminatorBeWalkedForEachWay(m_block.firstParameter()))
        return false;

    ways.reverse();
    ++walker.m_numberOfJoinsWalkedForEachWay;
    uint32_t namesAtHead = walker.inputBlock(m_frameIndex, block).firstNarrowerName;
    for (uint32_t place : ways)
        walkBlockOnWays(singleElementSpan(place), namesAtHead);

    frame().blocksWalkedForEachWay.shrink(0);
    walker.inputBlock(m_frameIndex, block).firstNarrowerName = namesAtHead;
    forgetValuesFrom(m_block.firstParameter());
    if (frame().valuesBuiltTwice)
        walker.forgetNamesOfWay(frame());
    walker.m_block = { };
    walker.m_state.clear();
    return true;
}

bool Walker::isWalkingBlockForEachWay(size_t frameIndex) const
{
    const Frame& frame = m_frames[frameIndex];
    return frame.blocksWalkedForEachWay.contains(frame.block);
}

bool Walker::inlineCalleeAsLinkOfChainOfItsOpcodes(const BodyValue& call)
{
    SetForScope callCopiedByLinkOfChain(m_callCopiedByLinkOfChain, call.id());
    SetForScope numberOfFramesAtThatCall(m_numberOfFramesAtCallCopiedByLinkOfChain, m_frames.size());
    return inlineCallee(*this, call);
}

bool Walker::armsTakeBranchForProofOf(BlockID inputBlock, uint32_t begin, uint32_t end, Reference value, const BitVector& placesThatTypedRunKnowsBetter)
{
    const Frame& frame = m_frames.last();
    BodyBlock block = frame.index->block(inputBlock);
    TerminatorKind kind = block.terminator().kind();
    bool returnsFromCopy = (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall) && !walksOwnBodyOfUnit() && !frame.returnsInPlaceOfUnit;
    bool mayBeTaken = goesByOneOperand(kind) || kind == TerminatorKind::Jump || returnsFromCopy;
    if ((value && !value.isValue()) || !mayBeTaken || frame.endOfStretch != block.numberOfValues() || block.numberOfValues() - end > mostOperationsOfJoinWalkedForEachWay)
        return false;
    if (hasTextsInStringBuilders())
        return false;
    if (!goesByOneOperand(kind))
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
    if (m_hasFailed || !has(Mechanism::Guesses) || !m_opensGuesses || m_numberOfCopiesBuiltForSize || hasTextsInStringBuilders())
        return false;
    return KnownThreading(*this, m_frames.last().block).letReturnsOfCopyWalkEndOfBlockApart(call, passing, closed);
}

void Walker::walkEndOfBlockBehindReturnsOfCopy(ValueID call)
{
    storeValueBehindItsDefinition(call);
    Frame& frame = m_frames.last();
    frame.walkedUpTo = call.index() - frame.index->block(frame.block).firstValue().index() + 1;
    if (!isUnreachable() && !endArmWithBranchOf(frame.block))
        fail("what is left of a block behind a copy is not walked by the ways that were to walk it apart"_s);
}

bool Walker::walkBlockApartFromClosedWays(BlockID inputBlock)
{
    if (m_hasFailed || !has(Mechanism::Guesses) || !m_opensGuesses || m_numberOfCopiesBuiltForSize || hasTextsInStringBuilders())
        return false;
    if (m_openEdges[this->inputBlock(m_frames.size() - 1, inputBlock).firstOpenEdge].next == noOpenEdge)
        return false;
    return KnownThreading(*this, inputBlock).walkApartFromClosedWays();
}

bool Walker::walkBlockApartFromWaysThatDidSomething(BlockID inputBlock)
{
    if (m_hasFailed || m_openLoops.isEmpty() || m_numberOfCopiesBuiltForSize || hasTextsInStringBuilders())
        return false;
    if (m_openEdges[this->inputBlock(m_frames.size() - 1, inputBlock).firstOpenEdge].next == noOpenEdge)
        return false;
    return KnownThreading(*this, inputBlock).walkApartFromWaysThatDidSomething();
}

bool Walker::walkBlockForEachWayThatBringsObjectInSight(BlockID inputBlock)
{
    if (m_hasFailed || !has(Mechanism::EscapeAnalysis) || !has(Mechanism::TypedOperations) || !m_opensGuesses || m_numberOfCopiesBuiltForSize || hasTextsInStringBuilders())
        return false;
    if (m_openEdges[this->inputBlock(m_frames.size() - 1, inputBlock).firstOpenEdge].next == noOpenEdge || !currentBodyIndex().block(inputBlock).numberOfParameters())
        return false;
    return KnownThreading(*this, inputBlock).walkForEachWayThatBringsObjectInSight();
}

bool Walker::endArmWithBranchOf(BlockID inputBlock)
{
    if (isUnreachable() || m_hasFailed)
        return false;
    return KnownThreading(*this, inputBlock).endArmWithBranch();
}

bool Walker::armsMayEndWithBranchBehind(const BodyValue& operation)
{
    const Frame& frame = m_frames.last();
    BodyBlock block = frame.index->block(frame.block);
    if (isUnreachable() || m_hasFailed || !block.holds(operation.id()) || hasTextsInStringBuilders())
        return false;
    return KnownThreading(*this, frame.block).armsMayWalkEndOfBlockFrom(operation.id().index() - block.firstValue().index() + 1);
}

bool Walker::endArmWithBranchBehind(const BodyValue& operation)
{
    storeValueBehindItsDefinition(operation.id());
    Frame& frame = m_frames.last();
    frame.walkedUpTo = operation.id().index() - frame.index->block(frame.block).firstValue().index() + 1;
    return endArmWithBranchOf(frame.block);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
