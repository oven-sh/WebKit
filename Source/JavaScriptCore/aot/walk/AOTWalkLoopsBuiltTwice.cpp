/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t largestLoopBuiltTwice = 100;

bool Walker::leavesLoopBeingBuiltTwice(size_t frameIndex, BlockID to) const
{
    if (!m_generatorsWalkedInConsumers.isEmpty() && leavesRegionOfIteration(frameIndex, to))
        return true;
    const Frame& frame = m_frames[frameIndex];
    return std::ranges::any_of(frame.loopsBeingBuiltTwice, [&](LoopID loop) {
        return loop.index() < frame.index->numberOfLoops() && !frame.index->isInLoop(to, loop);
    });
}

void Walker::takeNamesOntoEdge(OpenEdge& edge, size_t frameIndex)
{
    const Frame& frame = m_frames[frameIndex];
    if (!frame.valuesBuiltTwice)
        return;
    edge.names = frame.valuesBuiltTwice->namesOfWay;
    for (ValueID value : frame.valuesBuiltTwice->boundOnWay) {
        if (Reference name = frame.references[value.index()]; name && frame.valuesBuiltTwice->contains(value))
            edge.names.setNameIn(frame.valuesBuiltTwice->slotOf(value), name);
    }
}

void Walker::takeNamesOfWay(BlockID inputBlock, NamesOfWay&& names)
{
    Frame& frame = m_frames.last();
    frame.valuesBuiltTwice->namesOfWay = WTF::move(names);
    frame.valuesBuiltTwice->blockThatWayEnters = inputBlock;
    for (uint32_t carried = frame.index->numberOfValues(); carried < frame.references.size(); ++carried) {
        if (Reference name = frame.valuesBuiltTwice->nameOn(frame.valuesBuiltTwice->namesOfWay, ValueID(carried)))
            bind(ValueID(carried), name);
    }
    frame.valuesBuiltTwice->namesGivenForGood.removeAllMatching([&](const Frame::ValuesBuiltTwice::NamesGivenForGood& given) {
        if (given.block != inputBlock)
            return false;
        for (ValueID value : given.values) {
            Reference name = referenceBoundIn(frame, value);
            frame.valuesBuiltTwice->remove(value);
            if (name)
                bind(value, name);
        }
        return true;
    });
}

void Walker::forgetNamesOfWay(Frame& frame)
{
    Frame::ValuesBuiltTwice& listed = *frame.valuesBuiltTwice;
    for (ValueID value : listed.boundOnWay) {
        if (const Frame::ValuesBuiltTwice::Slot& slot = listed.slots[listed.slotOf(value)]; slot.ordinal && slot.hasSeveralNames && value.index() < frame.index->numberOfValues())
            frame.references[value.index()] = Reference();
    }
    listed.boundOnWay.shrink(0);
    listed.namesOfWay.clear();
    listed.blockThatWayEnters = { };
}

bool Walker::isNamedByWaysInto(const Frame& frame, ValueID value, BlockID inputBlock) const
{
    if (value == frame.stepWhosePositionEdgesBring)
        return true;
    if (!frame.isBuiltTwice(value))
        return false;
    if (value.index() >= frame.index->numberOfValues())
        return true;
    if (!m_liveness.isComputedFor(*frame.index))
        m_liveness.compute(*frame.index);
    return m_liveness.isLiveAtHead(value, inputBlock);
}

Reference Walker::nameBroughtByWay(const Frame& frame, ValueID value) const
{
    Reference name = frame.valuesBuiltTwice->nameOn(frame.valuesBuiltTwice->namesOfWay, value);
    return name && isNamedByWaysInto(frame, value, frame.valuesBuiltTwice->blockThatWayEnters) ? name : Reference();
}

bool Walker::isWithoutNameBehindLoopBuiltTwice(ValueID value)
{
    const Frame& frame = m_frames.last();
    if (!frame.isBuiltTwice(value) || frame.index->blockOf(value) == frame.block)
        return false;
    if (!m_liveness.isComputedFor(*frame.index))
        m_liveness.compute(*frame.index);
    return !m_liveness.isLiveAtHead(value, frame.block);
}

void Walker::walkCopyOfLoop(BlockID inputHead, std::span<const Reference> entering)
{
    SetForScope deeper(m_nestingDepth, m_nestingDepth + 1);
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);

    bool leavesFrontOpen = std::exchange(m_leavesFrontOfLoopOpenBehindWalk, false);
    uint32_t namesAtHead = inputBlock(frameIndex, inputHead).firstNarrowerName;
    for (BlockID member : index.membersOf(loop)) {
        if (!m_frames[frameIndex].blocks[member.index()].isAsNew())
            inputBlock(frameIndex, member) = InputBlock { };
    }
    inputBlock(frameIndex, inputHead).firstNarrowerName = namesAtHead;

    BlockID from = m_block;
    m_code.end(from, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
    uint32_t place = addOpenEdge(frameIndex, inputHead);
    if (place == noOpenEdge)
        return;
    OpenEdge& open = m_openEdges[place];
    open.edge = m_code.at(from).terminator.next();
    for (Reference argument : entering) {
        open.arguments.append(argument);
        open.types.append(ownTypeOf(argument));
    }
    open.state = WTF::move(m_state);
    takeNamesOntoEdge(open, frameIndex);
    m_block = { };
    NamesOfWay namesInFrontOfLoop;
    if (m_frames[frameIndex].valuesBuiltTwice)
        namesInFrontOfLoop = m_frames[frameIndex].valuesBuiltTwice->namesOfWay;

    size_t numberOfLoopsAround = m_openLoops.size();
    for (BlockID member : index.membersOf(loop)) {
        if (m_hasFailed)
            break;
        if (index.loopOf(member) != loop && m_frames[frameIndex].blocks[member.index()].firstOpenEdge == noOpenEdge)
            continue;
        walkBlock(member);
    }
    m_frontOfLoopLeftOpen = { };
    if (leavesFrontOpen && !m_hasFailed && m_openLoops.size() > numberOfLoopsAround) {
        endBlocksInFrontOfLoopsFrom(numberOfLoopsAround + 1);
        const OpenLoop& walked = m_openLoops.last();
        if (walked.frame == frameIndex && walked.loop == loop && walked.blockInFront && !m_code.at(walked.blockInFront).hasEnded()) {
            m_frontOfLoopLeftOpen = FrontOfLoopLeftOpen { walked.blockInFront, closeFrontOfQuietTurns(walked), walked.entering, walked.origin, walked.builtInFront };
            closeNewestLoop();
        }
    }
    endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
    m_block = { };
    m_state.clear();
    if (m_frames[frameIndex].valuesBuiltTwice) {
        forgetNamesOfWay(m_frames[frameIndex]);
        m_frames[frameIndex].valuesBuiltTwice->namesOfWay = WTF::move(namesInFrontOfLoop);
        m_frames[frameIndex].valuesBuiltTwice->blockThatWayEnters = inputHead;
    }
}

ValueID Walker::findReadAskedAtHeadOfLoop(BlockID inputHead, std::span<const std::pair<Reference, TypeID>> tested)
{
    const BodyIndex& index = currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    if (!has(Mechanism::TypedOperations) || !has(Mechanism::EscapeAnalysis) || hasTextsInStringBuilders() || m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames.last().inlineFrame, inputHead)))
        return { };
    for (BodyValue written : head.values()) {
        if (written.opcode() == Opcode::LoopHint)
            continue;
        if (written.opcode() != Opcode::PropGet || written.isFreeName())
            return { };
        ValueID input = written.objectOperand();
        Reference object = input && input.index() < head.firstParameter().index() ? referenceOf(input) : Reference();
        if (!object || !object.isValue() || isListAsMade(*this, object))
            return { };
        TypeID type = typeOf(object);
        for (auto [value, typeTested] : tested) {
            if (value == object && typeTested)
                type = m_types.intersectionOf(type, typeTested);
        }
        auto typeOfInput = [&](ValueID asked) {
            return asked == input ? type : TypeID();
        };
        return type && typedFormOf(*this, written, TypesOfInputs(typeOfInput)) == TypedForm::BuiltOnObjectAsMade ? written.id() : ValueID();
    }
    return { };
}

bool Walker::askObjectAtHeadOfTypedCopy(const BodyValue& read)
{
    const WayIntoClosedCopy way = m_waysIntoClosedCopies.last();
    size_t frameIndex = m_frames.size() - 1;
    if (way.frameIndex != frameIndex || m_frames[frameIndex].block != way.inputHead || isUnreachable() || hasTextsInStringBuilders())
        return false;
    if (findReadAskedAtHeadOfLoop(way.inputHead, { }) != read.id())
        return false;

    BodyBlock head = currentBodyIndex().block(way.inputHead);
    std::span<const ValueID> taken = m_code.at(way.entry).parameters();
    if (taken.size() != head.numberOfParameters() + numberOfFieldsOfScopesThatBlockTakes(way.entry))
        return false;
    Vector<Reference, 8> held;
    for (uint32_t index = 0; index < head.numberOfParameters(); ++index) {
        Reference reference = referenceOf(head.parameter(index));
        TypeID type = ownTypeOf(reference);
        if (!type || !m_types.isSubset(type, m_code.at(taken[index]).type))
            return false;
        held.append(reference);
    }

    Reference object = referenceOf(read.objectOperand());
    Reference test = buildTestOfObjectAsMade(*this, object, originOf(read));
    if (!test || m_hasFailed)
        return false;
    BlockID passed = addBlock();
    BlockID failed = addBlock(BlockFrequency::Rare);
    branch(test, passed, failed);
    if (failsGuesses()) [[unlikely]] {
        continueIn(passed);
        letGuessFailUnasked(failed, originOf(read));
        passed = currentBlock();
    }
    continueIn(failed);
    EdgeID noEdge;
    if (!isUnreachable() && bringFieldsOfScopesRound(noEdge, way.entry, held))
        endWithJumpInto(way.entry, held.span());
    continueIn(passed);
    if (m_hasFailed || isUnreachable())
        return false;
    if (m_types.at(typeOf(object)).hasObjects())
        noteListAsMade(*this, object);
    noteObjectPassedTestOfBeingAsMade(*this, object, test);
    noteGuessOpened();
    return true;
}

void Walker::noteHeadOfTurn(BlockID inputHead)
{
    m_state.noteSomethingWasDoneInTurn();
    if (m_waysIntoClosedCopies.isEmpty() || m_numberOfCopiesBuiltForSize || isUnreachable())
        return;
    WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (way.frameIndex != m_frames.size() - 1 || way.inputHead != inputHead)
        return;
    BodyBlock head = currentBodyIndex().block(inputHead);
    std::span<const ValueID> taken = m_code.at(way.entry).parameters();
    way.heldAtHead.shrink(0);
    std::span<const ValueID> heldByHead = m_code.at(m_block).parameters();
    size_t numberTaken = taken.size() - numberOfFieldsOfScopesThatBlockTakes(way.entry);
    if (numberTaken < head.numberOfParameters() || heldByHead.size() - numberOfFieldsOfScopesThatBlockTakes(m_block) != numberTaken)
        return;
    for (uint32_t index = 0; index < numberTaken; ++index) {
        Reference reference = index < head.numberOfParameters() ? referenceOf(head.parameter(index)) : Reference::value(heldByHead[index]);
        TypeID type = ownTypeOf(reference);
        if (!type || !m_types.isSubset(type, m_code.at(taken[index]).type)) {
            way.heldAtHead.shrink(0);
            return;
        }
        way.heldAtHead.append(reference);
    }
    way.isEntered = true;
    way.numberOfTurn = ++m_numberOfTypedCopiesEntered;
    m_state.noteHeadOfTurn(way.numberOfTurn);
}

bool Walker::askLesserBoundAt(const BodyValue& relation)
{
    WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (relation.id() != way.relationThatAsksLesserBound || way.frameIndex != m_frames.size() - 1)
        return false;
    way.headAsksLesserBound = false;
    way.testOfLesserBound = { };
    if (!way.lesserBound || !m_opensGuesses || m_frames.last().block != way.inputHead || !mayBeginTurnAgainInClosedCopy())
        return false;
    if (way.viewWhoseLengthIsBound && !findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, way.viewWhoseLengthIsBound))
        return false;
    Reference counter = referenceOf(relation.operand(0));
    if (!counter || !counter.isRecord() || !m_types.at(typeOf(counter)).isInteger())
        return false;
    if (failsGuesses()) [[unlikely]] {
        BlockID handsOver;
        letGuessFailUnasked(handsOver, originOf(relation));
        BlockID goesOn = currentBlock();
        if (handsOver) {
            continueIn(handsOver);
            if (!isUnreachable())
                beginTurnAgainInClosedCopy();
            continueIn(goesOn);
        }
    }
    Reference test = buildOperatorOnIntegersOfLoop(*this, relation.opcode(), counter, m_waysIntoClosedCopies.last().lesserBound, originOf(relation));
    if (!test || !test.isValue() || isUnreachable())
        return false;
    bind(relation.id(), test);
    m_waysIntoClosedCopies.last().headAsksLesserBound = true;
    m_waysIntoClosedCopies.last().testOfLesserBound = test;
    noteGuessOpened();
    count(StatisticID::CountersHeldBelowRoomOfViews);
    return true;
}

bool Walker::counterHoldsAccessBelowRoomOfView(ValueID access, Reference view) const
{
    if (m_waysIntoClosedCopies.isEmpty())
        return false;
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    return way.testOfLesserBound && way.frameIndex == m_frames.size() - 1 && way.accessesBelowRoomOfViews.get(access.index())
        && findKnown(*this, KnownNote::CounterLiesBelowLesserBound, way.testOfLesserBound) && findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view);
}

void Walker::proveWhatLooksCountOnAt(const BodyValue& operation)
{
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (!way.looksCountOnTurnsThatBeginAgain || way.frameIndex != m_frames.size() - 1 || !m_opensGuesses || !mayBeginTurnAgainInClosedCopy())
        return;
    auto typeAsBuilt = [&](ValueID input) -> TypeID {
        Reference built = referenceOf(input);
        TypeID known = built ? typeOf(built) : TypeID();
        return known ? known : TypeID(TypeTable::anything);
    };
    static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
    if (operators.contains(operation.opcode()) && typedFormOf(*this, operation, TypesOfInputs(typeAsBuilt)) != TypedForm::None) {
        bool proofPays = false;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            const Type& type = m_types.at(typeAsBuilt(operand));
            Reference built = referenceOf(operand);
            proofPays |= type.hasObjects() || (!type.isNumber() && (!built || !built.isRecord() || representationOf(built) == Representation::JSValue));
        });
        if (!proofPays)
            return;
    }
    Vector<std::pair<ValueID, TypeID>, 2> wanted;
    forEachTypeThatGuessWouldProveAt(*this, operation, TypesOfInputs(typeAsBuilt), [&](ValueID operand, TypeID expected, TypeID) {
        if (!guessAtCalleeAsksForBuiltinWhereItFails(*this, operation, operand))
            wanted.append({ operand, expected });
    });
    for (auto [operand, expected] : wanted) {
        if (mayBeginTurnAgainInClosedCopy())
            proveExpectedTypeOrBeginTurnAgain(*this, operand, expected, draftOf(operation).origin());
    }
}

bool Walker::turnsOfTypedCopyMayBeginAgain(BlockID inputHead, const LoopPreview& preview) const
{
    size_t frameIndex = m_frames.size() - 1;
    if (!preview.effects.mayRunOtherCode() || hasTextsInStringBuilders() || m_numberOfCopiesBuiltForSize || isUnderHandler()
        || (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !iterationsSteppedAtHandOverPositionsAlone(inputHead)))
        return false;
    return !preview.otherCodeRunsInLoopInside && currentBodyIndex().loop(currentBodyIndex().loopOf(inputHead)).numberOfLoopsWithin <= 1;
}

TypeID Walker::typeThatCounterIsHeldTo(BlockID inputHead, uint32_t parameter) const
{
    if (m_waysIntoClosedCopies.isEmpty())
        return { };
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (way.frameIndex != m_frames.size() - 1 || way.inputHead != inputHead)
        return { };
    for (auto [position, held] : way.countersHeld) {
        if (position == parameter)
            return held;
    }
    return { };
}

Reference Walker::askCounterAgainWhereItComesRound(EdgeID& edge, BlockID head, size_t parameter, std::span<const Reference> arguments)
{
    if (m_waysIntoClosedCopies.isEmpty())
        return { };
    BlockID inputHead = m_waysIntoClosedCopies.last().inputHead;
    BlockID entry = m_waysIntoClosedCopies.last().entry;
    TypeID held = typeThatCounterIsHeldTo(inputHead, static_cast<uint32_t>(parameter));
    if (!held || inputBlock(m_frames.size() - 1, inputHead).head != head)
        return { };
    std::span<const ValueID> taken = m_code.at(entry).parameters();
    std::span<const Reference> besideFieldsOfScopes = arguments.first(arguments.size() - numberOfFieldsOfScopesThatBlockTakes(head));
    if (taken.size() - numberOfFieldsOfScopesThatBlockTakes(entry) != besideFieldsOfScopes.size())
        return { };
    for (size_t index = 0; index < besideFieldsOfScopes.size(); ++index) {
        TypeID type = ownTypeOf(besideFieldsOfScopes[index]);
        if (!type || !m_types.isSubset(type, m_code.at(taken[index]).type))
            return { };
    }
    if (edge) {
        BlockID onEdge = m_code.addBlock(BlockRole::Ordinary, m_code.at(head).frequency);
        m_code.setLoopMarks(onEdge, m_code.at(head).isInLoop, false);
        m_code.closeEdge(edge, onEdge, { });
        m_block = onEdge;
        edge = { };
    }
    Reference test = buildTestOfNumber(*this, arguments[parameter], held, m_origin);
    if (!test || !test.isValue())
        return { };
    BlockID passed = addBlock();
    BlockID failed = addBlock(BlockFrequency::Rare);
    branch(test, passed, failed);
    continueIn(failed);
    Vector<Reference, 8> handedOn(besideFieldsOfScopes);
    EdgeID noEdge;
    if (!isUnreachable() && bringFieldsOfScopesRound(noEdge, entry, handedOn))
        endWithJumpInto(entry, handedOn.span());
    continueIn(passed);
    return isUnreachable() ? Reference() : buildProofOfNumber(*this, arguments[parameter], test, m_origin);
}

bool Walker::mayBeginTurnAgainInClosedCopy() const
{
    return !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().isEntered && !isUnreachable() && m_state.nothingWasDoneInTurn(m_waysIntoClosedCopies.last().numberOfTurn) && !isUnderHandler() && !hasTextsInStringBuilders();
}

bool Walker::comesRoundTypedCopy(Reference value)
{
    if (m_waysIntoClosedCopies.isEmpty() || !value.isValue())
        return false;
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    BlockID head = inputBlock(way.frameIndex, way.inputHead).head;
    return head && std::ranges::contains(m_code.at(head).parameters(), value.id());
}

std::optional<uint32_t> Walker::openLoopOfTypedCopy() const
{
    if (!mayBeginTurnAgainInClosedCopy() || !has(Mechanism::LoopInvariants))
        return std::nullopt;
    std::optional<uint32_t> innermost = innermostOpenLoop();
    if (!innermost)
        return std::nullopt;
    const OpenLoop& loop = m_openLoops[*innermost];
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (!way.startFromInFront || loop.frame != way.frameIndex || m_frames[loop.frame].index->loop(loop.loop).head != way.inputHead)
        return std::nullopt;
    if (!loop.blockInFront || m_code.at(loop.blockInFront).hasEnded() || m_code.at(loop.blockInFront).handler)
        return std::nullopt;
    return innermost;
}

bool Walker::standsInFrontOfTypedCopy(Reference reference) const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy();
    return loop && reference && reference.isRecord() && standsInFrontOfLoop(*loop, reference) && typeOf(reference) == ownTypeOf(reference);
}

bool Walker::turnsOfTypedCopyWriteNothingAboveValues() const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy();
    if (!loop)
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks }));
}

bool Walker::isBuiltInTurnThatMayBeginAgain(Reference reference) const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy();
    return loop && reference && reference.isValue() && !standsInFrontOfLoop(*loop, reference);
}

bool Walker::branchInFrontOfTypedCopy(Reference test, bool passesWithYes)
{
    std::optional<uint32_t> place = openLoopOfTypedCopy();
    if (!place || !test || !test.isValue() || !standsInFrontOfLoop(*place, test) || representationOf(test) != Representation::Boolean)
        return false;
    if (m_openLoops[*place].blocksOfFrontOfQuietTurns.contains(m_code.blockOf(test.id())))
        return branchInFrontOfQuietTurns(*place, test, passesWithYes);
    OpenLoop& loop = m_openLoops.changeable(*place);
    BlockID front = loop.blockInFront;
    BlockID next = m_code.addBlock(BlockRole::Ordinary, m_code.at(front).frequency);
    m_code.setLoopMarks(next, m_code.at(front).isInLoop, false);
    BlockID failing = m_waysIntoClosedCopies.last().startFromInFront;

    TerminatorDraft terminator(TerminatorKind::Branch, loop.origin);
    terminator.setConditionOperand(test);
    m_code.end(front, terminator, 2);
    m_code.closeEdge(m_code.at(front).terminator.taken(), passesWithYes ? next : failing, { });
    m_code.closeEdge(m_code.at(front).terminator.notTaken(), passesWithYes ? failing : next, { });

    while (m_placesOfLoopsBehindBlocks.size() <= next.index())
        m_placesOfLoopsBehindBlocks.append(0);
    setPlaceOfLoopBehindBlock(next, *place + 1);
    loop.earlierBlocksInFront.append(front);
    loop.blockInFront = next;
    return true;
}

Walker::InFrontOfTypedCopy::InFrontOfTypedCopy(Walker& walker, bool isInFront)
    : m_walker(walker)
    , m_isInFront(isInFront)
{
    if (!m_isInFront)
        return;
    uint32_t loop = *walker.openLoopOfTypedCopy();
    m_blockInside = std::exchange(walker.m_block, walker.m_openLoops[loop].blockInFront);
    m_numberOfLoopsThatMightBeLeft = std::exchange(walker.m_numberOfLoopsThatMayBeLeft, loop);
}

Walker::InFrontOfTypedCopy::~InFrontOfTypedCopy()
{
    if (!m_isInFront)
        return;
    m_walker.m_block = m_walker.m_hasFailed ? BlockID() : m_blockInside;
    m_walker.m_numberOfLoopsThatMayBeLeft = m_numberOfLoopsThatMightBeLeft;
}

bool Walker::looksCountOnTurnsThatBeginAgain(BlockID inputHead) const
{
    if (m_numberOfCopiesBuiltForSize || !m_opensGuesses || !has(Mechanism::Guesses) || isUnderHandler() || hasTextsInStringBuilders())
        return false;
    if (m_inputHeadAskedAboutTypedCopy == inputHead)
        return true;
    return !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().looksCountOnTurnsThatBeginAgain && m_waysIntoClosedCopies.last().frameIndex == m_frames.size() - 1 && m_waysIntoClosedCopies.last().inputHead == inputHead;
}

bool Walker::standsInTurnThatLooksCountOn() const
{
    if (m_looksTakeFormsBehindLooksAsBuilt)
        return true;
    return mayBeginTurnAgainInClosedCopy() && m_waysIntoClosedCopies.last().looksCountOnTurnsThatBeginAgain;
}

void Walker::beginTurnAgainInClosedCopy()
{
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    Vector<Reference, 8> held = way.heldAtHead;
    EdgeID noEdge;
    if (bringFieldsOfScopesRound(noEdge, way.entry, held))
        endWithJumpInto(way.entry, held.span());
    m_block = { };
    m_state.clear();
    ++m_numberOfTurnsThatBeginAgain;
}

bool Walker::isTooLargeToBeBuiltTwice(const BodyIndex& index, LoopID loop)
{
    uint32_t numberOfOperations = 0;
    for (BlockID member : index.membersOf(loop)) {
        numberOfOperations += index.block(member).numberOfValues() + 1;
        if (numberOfOperations > largestLoopBuiltTwice)
            return true;
    }
    return false;
}

bool Walker::mayBuildLoopSeveralTimes(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Guesses) || !m_opensGuesses || !index.isLoopHead(inputHead) || index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead))
        return false;
    if (index.holdsParametersReadByReleaseHintsAlone())
        return false;
    if (drawChoice(DrawnFor::LoopBuiltTwice, currentFunction().index(), inputHead.index()) == ChoiceOfCost::No)
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !iterationsSteppedAtMayBeBuiltTwice(inputHead))
        return false;

    if (isTooLargeToBeBuiltTwice(index, loop))
        return false;
    bool returnsLeaveCopy = frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit;
    bool bodyMaySuspend = index.holdsAnyOf(opcodesWithSuspendingBoundInTable) || index.holdsAnyOf(OpcodeSet { Opcode::IteratorResume, Opcode::IteratorClose });
    if (!returnsLeaveCopy && !bodyMaySuspend)
        return true;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        TerminatorKind end = block.terminator().kind();
        if (returnsLeaveCopy && (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall))
            return false;
        if (!bodyMaySuspend)
            continue;
        for (BodyValue operation : block.values()) {
            if (maySuspendAsWritten(operation))
                return false;
        }
    }
    return true;
}

uint32_t Walker::numberOfHandlersThatRead(Frame& frame, ValueID value)
{
    std::span<const BlockID> handlerBlocks = handlerBlocksOf(frame);
    if (handlerBlocks.empty())
        return 0;
    if (frame.numberOfHandlersThatRead.isEmpty()) {
        frame.numberOfHandlersThatRead.fill(0, frame.index->numberOfValues());
        for (BlockID handlerBlock : handlerBlocks) {
            m_liveness.forEachLiveAtHead(handlerBlock, [&](ValueID live) {
                ++frame.numberOfHandlersThatRead[live.index()];
            });
        }
    }
    return frame.numberOfHandlersThatRead[value.index()];
}

std::optional<Vector<ValueID, 8>> Walker::findValuesOfLoopReadBehindIt(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (m_valuesReadBehindLoopFoundLast.body == &index && m_valuesReadBehindLoopFoundLast.inputHead == inputHead)
        return m_valuesReadBehindLoopFoundLast.found;
    m_valuesReadBehindLoopFoundLast.body = &index;
    m_valuesReadBehindLoopFoundLast.inputHead = inputHead;
    m_valuesReadBehindLoopFoundLast.found = std::nullopt;
    LoopID loop = index.loopOf(inputHead);
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    ValueID endOfValues = index.block(index.loop(loop).last).endOfValues();
    Vector<ValueID, 8> readBehindLoop;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (index.isInLoop(target.block(), loop))
                continue;
            m_liveness.forEachLiveAtHead(target.block(), ValueID(firstValue), endOfValues, [&](ValueID live) {
                if (index.isInLoop(index.blockOf(live), loop) && !readBehindLoop.contains(live))
                    readBehindLoop.append(live);
            });
        }
    }
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value) {
            uint32_t numberOutsideLoop = numberOfHandlersThatRead(m_frames[frameIndex], ValueID(value));
            if (!numberOutsideLoop)
                continue;
            for (BlockID inside : index.membersOf(loop))
                numberOutsideLoop -= index.block(inside).isHandlerBlock() && m_liveness.isLiveAtHead(ValueID(value), inside);
            if (numberOutsideLoop)
                return std::nullopt;
        }
    }
    m_valuesReadBehindLoopFoundLast.found = readBehindLoop;
    return readBehindLoop;
}

void Walker::endFrontOfLoopLeftOpen()
{
    FrontOfLoopLeftOpen front = std::exchange(m_frontOfLoopLeftOpen, { });
    if (!front.block || m_code.at(front.block).hasEnded())
        return;
    m_code.end(front.block, TerminatorDraft(TerminatorKind::Jump, front.origin), 1);
    m_code.closeEdge(m_code.at(front.block).terminator.next(), front.head, front.entering.span());
}

Vector<uint32_t, 8> Walker::nameValuesOnEdgesThatLeavePlainWalk(BlockID inputHead, std::span<const ValueID> readBehindLoop)
{
    Vector<uint32_t, 8> edges;
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    for (ValueID value : readBehindLoop)
        m_frames[frameIndex].listAsBuiltTwice(value);
    Vector<BlockID, 8> entered;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            BlockID to = target.block();
            if (index.isInLoop(to, loop) || entered.contains(to))
                continue;
            entered.append(to);
            if (std::as_const(m_frames)[frameIndex].blocks[to.index()].isEntered)
                continue;
            InputBlock& behindLoop = inputBlock(frameIndex, to);
            behindLoop.firstNarrowerName = narrowerNamesOfDominator;
            for (uint32_t place = behindLoop.firstOpenEdge; place != noOpenEdge; place = std::as_const(m_openEdges)[place].next) {
                OpenEdge& open = m_openEdges[place];
                edges.append(place);
                if (open.block) {
                    open.edge = m_code.endWithJump(open.block, m_origin);
                    open.block = { };
                }
                const Frame& frame = m_frames[frameIndex];
                for (ValueID value : readBehindLoop) {
                    uint32_t slot = frame.valuesBuiltTwice->slotOf(value);
                    if (Reference bound = frame.references[value.index()]; bound && !open.names.nameIn(slot))
                        open.names.setNameIn(slot, bound);
                }
            }
        }
    }
    return edges;
}

void Walker::forgetValuesOfLoop(BlockID inputHead)
{
    Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        BodyBlock block = index.block(member);
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
            frame.references[value] = Reference();
    }
}

bool Walker::buildLoopForEachMember(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!m_buysTwoBuilds && !m_asksOnlyWhetherLoopMayBeBuiltTwice)
        return false;
    if (m_numberOfCopiesBuiltForSize || !m_closedCopiesShared.isEmpty() || (isUnreachable() && !m_asksOnlyWhetherLoopMayBeBuiltTwice) || hasTextsInStringBuilders())
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop) || m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)))
        return false;
    Vector<GuessInFrontOfLoop, 4> members = findMembersToldApartInFrontOfLoop(*this, inputHead, preview);
    if (members.size() < 2)
        return false;
    std::optional<Vector<ValueID, 8>> readBehindLoop = findValuesOfLoopReadBehindIt(inputHead);
    if (!readBehindLoop)
        return false;
    if (m_asksOnlyWhetherLoopMayBeBuiltTwice)
        return true;
    SetForScope isBought(m_buysTwoBuilds, false);
    SetForScope ordinalOfCopy(m_ordinalOfCopyOfLoop, m_ordinalOfCopyOfLoop * 16 + 3);

    struct StartOfCopy {
        BlockID block;
        WalkState state;
        Reference proof;
    };
    Origin origin = m_origin;
    Vector<StartOfCopy, 4> starts;
    for (const GuessInFrontOfLoop& member : members) {
        Reference test = isUnreachable() ? Reference() : buildTestInFrontOfLoop(*this, member, origin);
        if (!test)
            continue;
        BlockID passed = addBlock();
        BlockID next = addBlock();
        branch(test, passed, next);
        continueIn(passed);
        Reference proof = buildProofInFrontOfLoop(*this, member, test, origin);
        if (proof && !isUnreachable())
            starts.append(StartOfCopy { m_block, m_state, proof });
        continueIn(next);
    }
    if (m_hasFailed)
        return true;
    noteGuessOpened();
    ++m_numberOfLoopsBuiltTwice;

    BlockID closedStart = addBlock(BlockFrequency::Rare);
    jumpTo(closedStart, { });
    m_block = closedStart;
    BlockID entryOfClosedCopy = addBlock(BlockFrequency::Rare);
    Vector<Reference, 8> enteringClosedCopy;
    for (size_t place = 0; place < entering.size(); ++place) {
        TypeID type = joinTypes(preview.typesOfParameters[place], ownTypeOf(entering[place]));
        enteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
    }
    takeFieldsOfScopesIntoClosedCopy(entryOfClosedCopy, preview);
    settleParametersOf(entryOfClosedCopy);

    for (ValueID value : *readBehindLoop)
        m_frames[frameIndex].listAsBuiltTwice(value);
    m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
    uint32_t namesInFrontOfLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;

    forgetAllButFreshObjectsAndFieldsThatBlockTakes(entryOfClosedCopy);
    Vector<Reference, 8> enteringWithFields;
    enteringWithFields.append(entering);
    EdgeID noEdge;
    if (bringFieldsOfScopesRound(noEdge, entryOfClosedCopy, enteringWithFields))
        endWithJumpInto(entryOfClosedCopy, enteringWithFields.span());
    enterClosedCopy(entryOfClosedCopy, preview);
    {
        ClosedArm closedArm(*this);
        ++m_numberOfCopiesBuiltForSize;
        walkCopyOfLoop(inputHead, enteringClosedCopy.span());
        --m_numberOfCopiesBuiltForSize;
    }
    forgetValuesOfLoop(inputHead);

    m_closedCopiesShared.append(ClosedCopyShared { frameIndex, inputHead, closedStart, entryOfClosedCopy, false });
    TypesOfValuesOfLoop settledAroundCopies = m_frames[frameIndex].typesOfLooks;
    for (StartOfCopy& start : starts) {
        if (m_hasFailed)
            break;
        if (m_frames[frameIndex].loopLookedAt != loop)
            m_frames[frameIndex].typesOfLooks = settledAroundCopies;
        m_block = start.block;
        m_state = WTF::move(start.state);
        Vector<std::pair<ValueID, Reference>, 8> boundBefore;
        for (ValueID name : members[0].names) {
            boundBefore.append({ name, referenceBoundTo(name) });
            bind(name, start.proof);
        }
        m_closedCopiesShared.last().isAboutToBeTaken = true;
        m_waysIntoClosedCopies.append(WayIntoClosedCopy { frameIndex, inputHead, entryOfClosedCopy, { }, closedStart, { }, false });
        ++m_ordinalOfCopyOfLoop;
        walkCopyOfLoop(inputHead, entering);
        m_waysIntoClosedCopies.removeLast();
        m_closedCopiesShared.last().isAboutToBeTaken = false;
        for (auto [name, reference] : boundBefore)
            bind(name, reference);
        forgetValuesOfLoop(inputHead);
        forgetPlacesOfTextsInStringBuildersFrom(0);
        m_textsInStringBuilders.putBack(0, m_textsInStringBuilders.sizeOfLog());
    }
    m_closedCopiesShared.removeLast();

    for (BlockID member : index.membersOf(loop))
        inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
    m_frames[frameIndex].loopsBeingBuiltTwice.removeLast();
    m_block = { };
    m_state.clear();
    return true;
}

bool Walker::buildLoopTwice(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    if (!m_buysTwoBuilds && !m_asksOnlyWhetherLoopMayBeBuiltTwice)
        return false;
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    ClosedCopyShared* shared = !m_closedCopiesShared.isEmpty() && m_closedCopiesShared.last().frameIndex == frameIndex && m_closedCopiesShared.last().inputHead == inputHead && (m_asksOnlyWhetherLoopMayBeBuiltTwice ? m_closedCopiesShared.last().isAboutToBeTaken : std::exchange(m_closedCopiesShared.last().isAboutToBeTaken, false)) ? &m_closedCopiesShared.last() : nullptr;
    if (!shared && m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop))
        return false;
    BlockID sharedStart = shared ? shared->start : BlockID();
    BlockID sharedEntry = shared ? shared->entry : BlockID();
    bool sharedCopyIsPlainWalk = shared && shared->isPlainWalk;
    if (sharedCopyIsPlainWalk)
        m_closedCopiesShared.removeLast();

    SetForScope previewOutOfRow(m_previewOutOfRow, &preview);
    SetForScope isBought(m_buysTwoBuilds, false);
    IterationsSteppedAtHead steppedAtHead = takeIterationsSteppedAt(inputHead);
    auto takeIterationsAsTheyEntered = [&] {
        for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
            IterationByIndex& iteration = m_iterationsByIndex[place];
            iteration.mayGoByProtocol = entered.mayGoByProtocol;
            iteration.list = entered.list;
            iteration.state = entered.state;
            iteration.iterator = entered.iterator;
            iteration.next = entered.next;
            iteration.stateHoldsVerdictOnNext = entered.stateHoldsVerdictOnNext;
        }
    };
    auto takeIterationsAsSteppedByIndexAlone = [&] {
        for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
            if (!entered.mayGoByProtocol)
                continue;
            IterationByIndex& iteration = m_iterationsByIndex[place];
            iteration.mayGoByProtocol = false;
            iteration.state = { };
            iteration.iterator = { };
            iteration.next = { };
            iteration.stateHoldsVerdictOnNext = false;
        }
    };
    auto takeAsTheyEnteredAtEnd = makeScopeExit(takeIterationsAsTheyEntered);
    takeIterationsAsSteppedByIndexAlone();
    bool stepsWhatIsNoEnumeration = steppedAtHead.asTheyEntered.containsIf([](const auto& entered) { return entered.second.otherForm == IterationByIndex::OtherForm::None; });
    Vector<GuessInFrontOfLoop, 4> guessesAtListsAlone;
    ListsReadInLoop listsReadWithoutGuesses;
    if (stepsWhatIsNoEnumeration) {
        SetForScope asked(m_inputHeadAskedAboutTypedCopy, turnsOfTypedCopyMayBeginAgain(inputHead, preview) ? inputHead : m_inputHeadAskedAboutTypedCopy);
        listsReadWithoutGuesses = findListsReadInLoop(*this, inputHead, preview, entering, guessesAtListsAlone);
        if (!listsReadWithoutGuesses.areLeftAsTheyAre)
            return false;
    }
    Vector<GuessInFrontOfLoop, 4> guesses = findGuessesInFrontOfLoop(*this, inputHead, preview);
    BlockID headOfLooksTriedForLists = turnsOfTypedCopyMayBeginAgain(inputHead, preview) ? inputHead : m_inputHeadAskedAboutTypedCopy;
    ListsReadInLoop listsRead;
    if (stepsWhatIsNoEnumeration && guesses.isEmpty()) {
        listsRead = WTF::move(listsReadWithoutGuesses);
        guesses = WTF::move(guessesAtListsAlone);
    } else {
        SetForScope asked(m_inputHeadAskedAboutTypedCopy, headOfLooksTriedForLists);
        listsRead = findListsReadInLoop(*this, inputHead, preview, entering, guesses);
    }
    Vector<CounterTestedInFrontOfLoop, 2> counters = findCountersTestedInFrontOfLoop(*this, inputHead, preview, entering, guesses);
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        if (entered.mayGoByProtocol && !guesses.containsIf([&](const GuessInFrontOfLoop& asked) { return asked.value == entered.list; }))
            return false;
    }
    Vector<std::pair<Reference, TypeID>, 4> testedInFront;
    for (const GuessInFrontOfLoop& guess : guesses)
        testedInFront.append({ guess.value, guess.tested });
    bool asksAtHead = !!findReadAskedAtHeadOfLoop(inputHead, testedInFront.span());
    Vector<VariableID, 4> variablesCheckedInTurns;
    for (BlockID member : index.membersOf(loop)) {
        if (!has(Mechanism::Folding) || m_numberOfCopiesBuiltForSize)
            break;
        for (BodyValue operation : index.block(member).values()) {
            ValueID read = operation.opcode() == Opcode::CheckInitialized ? index.valueBehindCasts(operation.operand(0)) : ValueID();
            if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
                continue;
            VariableID variable = index.value(read).variable();
            if (variablesCheckedInTurns.contains(variable) || !leavesDeadZoneForGood(program(), variable) || m_state.isPastDeadZone(variable))
                continue;
            const DeferredVariable* deferred = plan().deferredDefinitions().findDeferred(variable);
            if (deferred && deferred->mark == Primitive::Absent)
                continue;
            TypeID widest = widestTypeOfOperation(*this, index.value(read));
            if (widest && (m_types.at(widest).primitives & Primitive::Absent))
                variablesCheckedInTurns.append(variable);
        }
    }
    bool closedCopyHasEntry = sharedEntry || asksAtHead || (!hasTextsInStringBuilders() && (steppedAtHead.asTheyEntered.isEmpty() ? !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) : iterationsSteppedAtHandOverPositionsAlone(inputHead)));
    auto isHeldWhereItComesRound = [](const CounterTestedInFrontOfLoop& counter) {
        return counter.isHeldWhereItComesRound;
    };
    bool closedCopyTakesWhatComesRound = closedCopyHasEntry && steppedAtHead.asTheyEntered.isEmpty();
    if (closedCopyTakesWhatComesRound && counters.containsIf(isHeldWhereItComesRound) && mayKeepTextsInStringBuilders(*this) && !preview.effects.maySuspend) {
        Vector<GrowingText, 2> texts;
        findGrowingTexts(*this, inputHead, preview, texts);
        closedCopyTakesWhatComesRound = texts.isEmpty();
    }
    if (!closedCopyTakesWhatComesRound)
        counters.removeAllMatching(isHeldWhereItComesRound);
    bool hasTestInFront = !guesses.isEmpty() || !listsRead.lists.isEmpty() || !counters.isEmpty() || asksAtHead;
    bool otherCopyOpensItsGuesses = guesses.isEmpty() && counters.isEmpty() && !asksAtHead;
    Vector<MemberConditionRecord, 4> guardsAskedOfObjectsOfTurns;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::PropGet || operation.isFreeName())
                continue;
            ValueID object = index.valueBehindCasts(operation.objectOperand());
            if (index.isParameter(object) || index.opcodeOf(object) != Opcode::Construct || !index.isInLoop(index.blockOf(object), loop))
                continue;
            FunctionID constructor = facts().linked().calleeAt(currentFunction(), object);
            if (TypeID made = constructor ? typeOfObjectsMadeBy(*this, constructor) : TypeID())
                findGuardsOfMethodAboveObjectsOf(*this, made, operation.key(), guardsAskedOfObjectsOfTurns);
        }
    }
    hasTestInFront |= !guardsAskedOfObjectsOfTurns.isEmpty();
    otherCopyOpensItsGuesses &= guardsAskedOfObjectsOfTurns.isEmpty();
    auto typeInRow = [&](ValueID input) -> TypeID {
        TypeID known;
        if (input.index() >= preview.firstValue && input.index() - preview.firstValue < preview.typesOfValues.size())
            known = preview.typesOfValues[input.index() - preview.firstValue];
        else if (Reference built = referenceOf(input))
            known = typeOf(built);
        return known ? known : TypeID(TypeTable::anything);
    };
    auto everyOperationThatRanOtherCodeMayBeTyped = [&] {
        if (m_operationsThatRanOtherCodeOfRow.isEmpty())
            return false;
        for (ValueID ran : m_operationsThatRanOtherCodeOfRow) {
            if (!index.isInLoop(index.blockOf(ran), loop))
                return false;
            BodyValue operation = index.value(ran);
            bool storesIntoListTested = operation.opcode() == Opcode::ElemSet && listsRead.lists.containsIf([](const ListTestedInFrontOfLoop& list) { return list.isWritable; });
            bool readsListTested = !listsRead.lists.isEmpty() && (operation.opcode() == Opcode::ElemGet || storesIntoListTested || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == program().keyOf(BuiltinNames::length)) || findListSteppedByIndexAlone(*this, index, operation));
            if (!readsListTested && effectRuleOf(operation.opcode()) != EffectRule::Operator && !guessesMayTypeOperationInCleanTurn(*this, operation, TypesOfInputs(typeInRow)))
                return false;
        }
        return true;
    };
    std::unique_ptr<LoopPreview> ofTypedCopy;
    if (turnsOfTypedCopyMayBeginAgain(inputHead, preview) && (m_rowSaysTurnsAreQuietOutsideArms || everyOperationThatRanOtherCodeMayBeTyped() || m_firstOtherCodeRanInCopyOfRow)) {
        ofTypedCopy = makeUniqueWithoutFastMallocCheck<LoopPreview>();
        ofTypedCopy->effects = m_effectsOutsideArmsOfRow;
        ofTypedCopy->typesOfParameters = preview.typesOfParameters;
        ofTypedCopy->typesOfValues = preview.typesOfValues;
        ofTypedCopy->firstValue = preview.firstValue;
        ofTypedCopy->guessesCountedOn = m_testsSeenOfRow;
        ofTypedCopy->hasLetMentionsLeave = true;
        Vector<std::pair<ValueID, TypeID>, 2> membersAbovePrimitives;
        bool asksMembersByTheirBytes = false;
        for (const GuessCountedOnByLooks& seen : m_testsSeenOfRow) {
            BodyValue read = index.value(seen.operation);
            if (read.opcode() != Opcode::PropGet || read.objectOperand() != seen.operand)
                continue;
            if (typeOfMemberAskedByItsByte(*this, read, typeInRow(seen.operand))) {
                asksMembersByTheirBytes = true;
                continue;
            }
            if (read.isFreeName())
                continue;
            if (TypeID member = typeFoundAbovePrimitiveBehindWatch(*this, read, typeInRow(seen.operand)))
                membersAbovePrimitives.append({ seen.operation, member });
        }
        Vector<TypeID> membersAskedByTheirBytes;
        auto memberAskedByItsByte = [&](ValueID input) -> TypeID {
            ValueID read = index.valueBehindCasts(input);
            return read.index() >= preview.firstValue && read.index() - preview.firstValue < membersAskedByTheirBytes.size() ? membersAskedByTheirBytes[read.index() - preview.firstValue] : TypeID();
        };
        if (asksMembersByTheirBytes) {
            membersAskedByTheirBytes.grow(preview.typesOfValues.size());
            for (BlockID member : index.membersOf(loop)) {
                for (BodyValue read : index.block(member).values()) {
                    if (read.opcode() != Opcode::PropGet || read.id().index() < preview.firstValue || read.id().index() - preview.firstValue >= membersAskedByTheirBytes.size())
                        continue;
                    TypeID holder = memberAskedByItsByte(read.objectOperand());
                    membersAskedByTheirBytes[read.id().index() - preview.firstValue] = typeOfMemberAskedByItsByte(*this, read, holder ? holder : typeInRow(read.objectOperand()));
                }
            }
        }
        if (!membersAbovePrimitives.isEmpty() || asksMembersByTheirBytes) {
            ofTypedCopy->countsOnMembersAbovePrimitives = true;
            auto typeBehindWatch = [&](ValueID input) -> TypeID {
                for (auto [read, member] : membersAbovePrimitives) {
                    if (read == input)
                        return member;
                }
                if (TypeID member = memberAskedByItsByte(input))
                    return member;
                return typeInRow(input);
            };
            ofTypedCopy->guessesCountedOn.removeAllMatching([&](const GuessCountedOnByLooks& seen) {
                if (membersAbovePrimitives.containsIf([&](const auto& found) { return found.first == seen.operation; }) || memberAskedByItsByte(seen.operation))
                    return true;
                ValueID made = index.valueBehindCasts(seen.operand);
                return !index.isParameter(made) && index.opcodeOf(made) == Opcode::Call && wouldCallBuiltin(*this, index.value(made), TypesOfInputs(typeBehindWatch)) != BuiltinCallAskedAbout::None;
            });
        }
        if (mayKeepTextsInStringBuilders(*this)) {
            Vector<GrowingText, 2> texts;
            findGrowingTexts(*this, inputHead, *ofTypedCopy, texts);
            if (!texts.isEmpty())
                ofTypedCopy = nullptr;
        }
    }
    bool typedCopyIsQuietByRow = !!ofTypedCopy;
    Effects effectsOfTypedCopyByRow = m_effectsOutsideArmsOfRow;
    Vector<FieldOfScope, 4> fieldsTestedInFront;
    if (closedCopyHasEntry && !sharedEntry && steppedAtHead.asTheyEntered.isEmpty())
        findFieldsOfScopesFromOutsideThatQuietTurnsStore(inputHead, ofTypedCopy ? ofTypedCopy->effects : preview.effects, fieldsTestedInFront, false);
    auto staysInt32ByRow = [&](const FieldOfScope& field) {
        TypeID read = resultTypeOf(draftOfReadOf(field));
        return field.isSteppedOrAddedTo && !m_types.isSubset(read, TypeTable::int32) && m_types.isSubset(TypeTable::int32, read) && std::ranges::all_of(field.addedInTurns, [&](ValueID added) {
            return m_types.isSubset(typeInRow(added), TypeTable::int32);
        });
    };
    fieldsTestedInFront.removeAllMatching([&](const FieldOfScope& field) {
        TypeID read = resultTypeOf(draftOfReadOf(field));
        if (!read || !m_types.overlap(read, TypeTable::number))
            return true;
        if (std::ranges::any_of(field.storedInTurns, [&](ValueID stored) {
            TypeID byRow = typeInRow(stored);
            return byRow && !m_types.overlap(byRow, TypeTable::number);
        }))
            return true;
        if (!m_types.at(read).isNumber())
            return !field.isComputedFromItself;
        return !staysInt32ByRow(field) && !mayBeInDeadZone(field);
    });
    for (FieldOfScope& field : fieldsTestedInFront)
        field.type = staysInt32ByRow(field) ? TypeID(TypeTable::int32) : m_types.intersectionOf(resultTypeOf(draftOfReadOf(field)), TypeTable::number);
    hasTestInFront |= !fieldsTestedInFront.isEmpty();
    otherCopyOpensItsGuesses &= fieldsTestedInFront.isEmpty();
    CounterHeldBelowRoomOfViews heldBelowRoom;
    bool storesUnderCounterAloneRanOtherCode = false;
    if (const Effects& ofTurns = ofTypedCopy ? ofTypedCopy->effects : preview.effects; closedCopyHasEntry && steppedAtHead.asTheyEntered.isEmpty() && !asksAtHead && !testsEveryPosition() && !m_numberOfCopiesBuiltForSize
        && has(Mechanism::TypedOperations) && has(Mechanism::UnboxedNumbers) && !isUnderHandler() && !ofTurns.maySuspend
        && (ofTurns.mayRunOtherCode() ? !ofTypedCopy && !m_operationsThatRanOtherCodeOfRow.isEmpty() : !ofTurns.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage })))) {
        auto typeThatEnters = [&](uint32_t position) -> TypeID {
            for (const CounterTestedInFrontOfLoop& counter : counters) {
                if (counter.position == position)
                    return counter.tested;
            }
            return position < entering.size() && entering[position] && entering[position].isRecord() ? typeOf(entering[position]) : TypeID();
        };
        auto typeInFront = [&](ValueID written) -> TypeID {
            Reference built = referenceOf(written);
            if (!built || !built.isRecord())
                return { };
            for (const GuessInFrontOfLoop& guess : guesses) {
                if (guess.value == built && guess.tested)
                    return guess.tested;
            }
            return typeOf(built);
        };
        heldBelowRoom = findCounterHeldBelowRoomOfViews(*this, inputHead, scopedLambda<TypeID(uint32_t)>(typeThatEnters), scopedLambda<TypeID(ValueID)>(typeInFront));
        if (ofTurns.mayRunOtherCode() && !heldBelowRoom.accesses.isEmpty()) {
            BitVector isUnderCounter;
            for (const AccessUnderCounterOfLoop& access : heldBelowRoom.accesses)
                isUnderCounter.set(access.access.index());
            storesUnderCounterAloneRanOtherCode = std::ranges::all_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
                if (!isUnderCounter.get(ran.index()) || index.opcodeOf(ran) != Opcode::ElemSet)
                    return false;
                ValueID stored = index.valueBehindCasts(index.value(ran).valueOperand());
                return isUnderCounter.get(stored.index()) && !index.isParameter(stored) && index.opcodeOf(stored) == Opcode::ElemGet;
            });
            if (!storesUnderCounterAloneRanOtherCode)
                heldBelowRoom.accesses.shrink(0);
        }
    }
    bool holdsCounterBelowRoom = !heldBelowRoom.accesses.isEmpty();
    if (!hasTestInFront && variablesCheckedInTurns.isEmpty() && !ofTypedCopy && !holdsCounterBelowRoom)
        return false;
    otherCopyOpensItsGuesses &= variablesCheckedInTurns.isEmpty() || ofTypedCopy || !listsRead.lists.isEmpty();
    otherCopyOpensItsGuesses &= !ofTypedCopy || !ofTypedCopy->countsOnMembersAbovePrimitives || !ofTypedCopy->guessesCountedOn.isEmpty();
    for (size_t place = 0; ofTypedCopy && place < ofTypedCopy->guessesCountedOn.size(); ++place) {
        ValueID parameter = index.valueBehindCasts(ofTypedCopy->guessesCountedOn[place].operand);
        BodyBlock head = index.block(inputHead);
        if (!index.isParameter(parameter) || index.blockOf(parameter) != inputHead)
            continue;
        uint32_t position = parameter.index() - head.firstParameter().index();
        if (position >= entering.size())
            continue;
        Reference enters = entering[position];
        bool isTestedAlready = std::ranges::find(entering.first(position), enters) != entering.first(position).end()
            || counters.containsIf([&](const CounterTestedInFrontOfLoop& counter) { return counter.position == position; })
            || guesses.containsIf([&](const GuessInFrontOfLoop& guess) { return guess.value == enters; });
        if (isTestedAlready)
            continue;
        if (TypeID tested = typeThatTypeTestAsksForWhereExpected(*this, enters, ofTypedCopy->guessesCountedOn[place].expected))
            counters.append({ position, tested });
    }
    if ((!sharedEntry || sharedCopyIsPlainWalk) && !hasTestInFront && variablesCheckedInTurns.isEmpty() && counters.isEmpty() && !holdsCounterBelowRoom) {
        uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
        const Effects& ofTurns = ofTypedCopy->effects;
        auto isTheSameInEveryTurn = [&](auto& self, ValueID written, unsigned depth) -> bool {
            ValueID value = index.valueBehindCasts(written);
            if (value.index() < firstValueOfLoop)
                return true;
            if (!depth || index.isParameter(value))
                return false;
            BodyValue definition = index.value(value);
            if (definition.opcode() == Opcode::CheckInitialized)
                return self(self, definition.operand(0), depth - 1);
            if (definition.opcode() == Opcode::GetModuleVariable)
                return !declaredEffectsOfOperation(program(), Opcode::GetModuleVariable, definition).reads.overlaps(ofTurns.writes);
            if (definition.opcode() == Opcode::GetEnvironmentField)
                return !declaredEffectsOfOperation(program(), Opcode::GetEnvironmentField, definition).reads.overlaps(ofTurns.writes) && self(self, definition.environmentOperand(), depth - 1);
            if (definition.opcode() != Opcode::PropGet || definition.isFreeName())
                return false;
            LocationMasks slot;
            slot.add(program(), MemoryLocation::objectField(definition.key()));
            if (slot.overlaps(ofTurns.writes))
                return false;
            return self(self, definition.objectOperand(), depth - 1);
        };
        bool somethingLeavesTheTurns = ofTypedCopy->countsOnMembersAbovePrimitives || ofTypedCopy->guessesCountedOn.containsIf([&](const GuessCountedOnByLooks& guess) {
            return isTheSameInEveryTurn(isTheSameInEveryTurn, guess.operand, 4);
        });
        if (!somethingLeavesTheTurns && !m_firstOtherCodeRanInCopyOfRow)
            return false;
    }

    std::optional<Vector<ValueID, 8>> foundBehindLoop = findValuesOfLoopReadBehindIt(inputHead);
    if (!foundBehindLoop)
        return false;
    if (m_asksOnlyWhetherLoopMayBeBuiltTwice) {
        m_otherCopyOpensItsGuessesAsAsked = otherCopyOpensItsGuesses;
        return true;
    }
    Vector<ValueID, 8> readBehindLoop = WTF::move(*foundBehindLoop);

    Origin origin = m_origin;
    BlockID closedStart = sharedStart;
    bool checksVariableInFront = false;
    bool rareWayLeadsToClosedStart = false;
    for (VariableID variable : variablesCheckedInTurns) {
        Draft read(Opcode::GetModuleVariable, origin);
        read.setVariable(variable);
        Reference found = isUnreachable() ? Reference() : build(WTF::move(read));
        if (!found || !found.isValue() || !(m_types.at(typeOf(found)).primitives & Primitive::Absent))
            continue;
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(found);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
        Reference isEmpty = build(WTF::move(test));
        if (!isEmpty || !isEmpty.isValue())
            continue;
        BlockID filled = addBlock();
        if (!closedStart)
            closedStart = addBlock(BlockFrequency::Rare);
        branch(isEmpty, closedStart, filled);
        checksVariableInFront = true;
        rareWayLeadsToClosedStart = true;
        continueIn(filled);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(found);
        narrowed.setTestOperand(isEmpty);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(false);
        Reference held = build(WTF::move(narrowed));
        if (!held)
            continue;
        m_state.noteEndOfDeadZone(variable);
        if (takeObjectOfDeferredVariable(*this, variable, held, origin))
            continue;
        m_state.noteVariable(variable, held);
        noteValueOfStableBinding(*this, variable, held);
    }
    if ((sharedCopyIsPlainWalk ? !checksVariableInFront : closedStart == sharedStart) && !hasTestInFront && !ofTypedCopy && !holdsCounterBelowRoom)
        return false;
    letGuessFailUnasked(closedStart, origin);
    if (!guardsAskedOfObjectsOfTurns.isEmpty() && !isUnreachable()) {
        Vector<LayoutID, 4> begun;
        for (const MemberConditionRecord& condition : guardsAskedOfObjectsOfTurns) {
            if (!program().at(condition.layout).has(LayoutFlags::IsConstructorFunction) || begun.contains(condition.layout))
                continue;
            begun.append(condition.layout);
            Draft begin(Opcode::BeginMemberGuards, origin);
            begin.setLayout(condition.layout);
            build(WTF::move(begin));
        }
        Draft test(Opcode::AreMembersUnchanged, origin);
        test.setConditions(guardsAskedOfObjectsOfTurns.span());
        if (Reference holds = build(WTF::move(test))) {
            BlockID passed = addBlock();
            if (!closedStart)
                closedStart = addBlock(BlockFrequency::Rare);
            branch(holds, passed, closedStart);
            rareWayLeadsToClosedStart = true;
            continueIn(passed);
            for (const MemberConditionRecord& condition : guardsAskedOfObjectsOfTurns)
                m_state.noteGuardOfReadsHolds(condition.layout, condition.key, holds);
        }
    }
    Vector<Reference, 4> proofs;
    for (const GuessInFrontOfLoop& guess : guesses) {
        Reference test = isUnreachable() ? Reference() : buildTestInFrontOfLoop(*this, guess, origin);
        if (!test) {
            proofs.append(Reference());
            continue;
        }
        BlockID passed = addBlock();
        if (!closedStart)
            closedStart = addBlock(BlockFrequency::Rare);
        branch(test, passed, closedStart);
        rareWayLeadsToClosedStart = true;
        continueIn(passed);
        proofs.append(buildProofInFrontOfLoop(*this, guess, test, origin));
    }
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        size_t guess = entered.mayGoByProtocol ? guesses.findIf([&](const GuessInFrontOfLoop& asked) { return asked.value == entered.list; }) : notFound;
        if (guess != notFound && guess < proofs.size() && proofs[guess])
            m_iterationsByIndex[place].list = proofs[guess];
    }
    Vector<Reference, 8> enteringTypedCopy;
    enteringTypedCopy.append(entering);
    for (const CounterTestedInFrontOfLoop& counter : counters) {
        Reference test = isUnreachable() ? Reference() : buildTestOfNumber(*this, entering[counter.position], counter.tested, origin);
        if (!test)
            continue;
        BlockID passed = addBlock();
        if (!closedStart)
            closedStart = addBlock(BlockFrequency::Rare);
        branch(test, passed, closedStart);
        rareWayLeadsToClosedStart = true;
        continueIn(passed);
        enteringTypedCopy[counter.position] = buildProofOfNumber(*this, entering[counter.position], test, origin);
    }
    for (const FieldOfScope& field : fieldsTestedInFront) {
        Reference read = isUnreachable() ? Reference() : valueKnownOf(field);
        if (!read && !isUnreachable()) {
            Draft inFront = draftOfReadOf(field);
            inFront.setFollowsItsStore(false);
            read = build(WTF::move(inFront));
        }
        Reference test = read ? buildTestOfNumber(*this, read, field.type, origin) : Reference();
        if (!test || !test.isValue())
            continue;
        BlockID passed = addBlock();
        if (!closedStart)
            closedStart = addBlock(BlockFrequency::Rare);
        branch(test, passed, closedStart);
        continueIn(passed);
        Reference proof = buildProofOfNumber(*this, read, test, origin);
        if (proof && field.variable) {
            m_state.noteEndOfDeadZone(field.variable);
            m_state.noteVariable(field.variable, proof);
        } else if (proof)
            noteEnvironmentField(field.scope, field.field, proof);
    }
    Reference lesserBound;
    Reference viewWhoseLengthIsBound;
    BitVector accessesBelowRoomOfViews;
    if (holdsCounterBelowRoom && !isUnreachable()) {
        uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
        auto builtInFront = [&](ValueID written) -> Reference {
            ValueID value = index.valueBehindCasts(written);
            if (value.index() >= firstValueOfLoop) {
                Draft integer(Opcode::Integer, origin);
                integer.setInteger(index.value(value).as<Opcode::Integer>().integer());
                return constant(WTF::move(integer));
            }
            Reference built = referenceOf(written.index() < firstValueOfLoop ? written : value);
            for (size_t place = 0; place < guesses.size(); ++place) {
                if (guesses[place].value == built && proofs[place])
                    return proofs[place];
            }
            return built;
        };
        auto isInteger = [&](Reference value) {
            return value && value.isRecord() && m_types.at(typeOf(value)).isInteger();
        };
        UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> askedForSign;
        auto goOnWhereItIsNotNegative = [&](Reference asked) {
            if (isUnreachable() || !isInteger(asked))
                return false;
            if (m_types.at(typeOf(asked)).integers.minimum >= 0 || !askedForSign.add(asked.bits()).isNewEntry)
                return true;
            Draft zero(Opcode::Integer, origin);
            zero.setInteger(0);
            Reference test = buildOperatorOnIntegersOfLoop(*this, Opcode::GreaterEq, asked, constant(WTF::move(zero)), origin);
            if (!test || !test.isValue())
                return false;
            BlockID passed = addBlock();
            if (!closedStart)
                closedStart = addBlock(BlockFrequency::Rare);
            branch(test, passed, closedStart);
            rareWayLeadsToClosedStart = true;
            continueIn(passed);
            return !isUnreachable();
        };
        auto lesserOf = [&](Reference one, Reference other) -> Reference {
            Reference isLess = buildOperatorOnIntegersOfLoop(*this, Opcode::Less, one, other, origin);
            if (!isLess || !isLess.isValue() || isUnreachable())
                return { };
            BlockID takesOne = addBlock();
            BlockID takesOther = addBlock();
            branch(isLess, takesOne, takesOther);
            continueIn(takesOther);
            if (other.isValue()) {
                Draft narrowed(Opcode::Narrowed, origin);
                narrowed.setValueOperand(other);
                narrowed.setTestOperand(isLess);
                narrowed.setOperandIndex(OperandIndex::Less::right);
                narrowed.setIsOnTakenEdge(false);
                if (Reference held = build(WTF::move(narrowed)))
                    other = held;
            }
            Vector<ArmThatMeets, 2> arms;
            arms.append(ArmThatMeets { takesOne, { one } });
            arms.append(ArmThatMeets { takesOther, { other } });
            Reference lesser;
            return letArmsMeet(arms.span(), addBlock(), singleElementSpan(lesser)) ? lesser : Reference();
        };
        Reference enters = enteringTypedCopy[heldBelowRoom.position];
        Reference bound = heldBelowRoom.viewWhoseLengthIsBound ? buildRoomOfViewBesideAddend(*this, builtInFront(heldBelowRoom.viewWhoseLengthIsBound), { }, origin) : builtInFront(heldBelowRoom.bound);
        bool stepsUp = isInteger(enters) && isInteger(bound) && (!heldBelowRoom.stepThatMayBeNegative || goOnWhereItIsNotNegative(builtInFront(heldBelowRoom.stepThatMayBeNegative)));
        UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> asked;
        UncheckedKeyHashMap<uint32_t, Range, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> writtenAddendsOfViews;
        auto writtenAddend = [&](const AccessUnderCounterOfLoop& access) -> std::optional<int64_t> {
            if (!access.addend)
                return 0;
            ValueID value = index.valueBehindCasts(access.addend);
            if (value.index() < firstValueOfLoop)
                return std::nullopt;
            return index.value(value).as<Opcode::Integer>().integer();
        };
        auto integerOf = [&](int64_t value) -> Reference {
            if (!value)
                return { };
            Draft integer(Opcode::Integer, origin);
            integer.setInteger(value);
            return constant(WTF::move(integer));
        };
        for (const AccessUnderCounterOfLoop& access : heldBelowRoom.accesses) {
            Reference view = builtInFront(access.view);
            if (std::optional<int64_t> written = view ? writtenAddend(access) : std::nullopt) {
                Range& range = writtenAddendsOfViews.add(view.bits(), Range { *written, *written }).iterator->value;
                range = Range { std::min(range.minimum, *written), std::max(range.maximum, *written) };
            }
        }
        Reference leastRoom;
        for (size_t place = 0; stepsUp && !m_hasFailed && place < heldBelowRoom.accesses.size(); ++place) {
            const AccessUnderCounterOfLoop& access = heldBelowRoom.accesses[place];
            Reference view = builtInFront(access.view);
            bool isWritten = view && writtenAddend(access);
            Reference addend = isWritten ? integerOf(writtenAddendsOfViews.get(view.bits()).maximum) : builtInFront(access.addend);
            if (!view || !view.isValue() || !isViewOfNumbersReadInLine(*this, typeOf(view)) || (addend && !isInteger(addend)) || (!isWritten && !addend))
                continue;
            uint64_t viewAndAddend = static_cast<uint64_t>(view.bits()) << 32 | addend.bits();
            if (!asked.contains(viewAndAddend)) {
                Reference leastAddend = isWritten ? integerOf(writtenAddendsOfViews.get(view.bits()).minimum) : addend;
                if (!goOnWhereItIsNotNegative(leastAddend ? buildOperatorOnIntegersOfLoop(*this, Opcode::Add, enters, leastAddend, origin) : enters))
                    continue;
                Reference room = buildRoomOfViewBesideAddend(*this, view, addend, origin);
                Reference least = room && leastRoom && !isUnreachable() ? lesserOf(room, leastRoom) : room;
                if (!least || isUnreachable())
                    continue;
                leastRoom = least;
                asked.add(viewAndAddend);
                noteKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), 1, true);
            }
            accessesBelowRoomOfViews.set(access.access.index());
        }
        if (leastRoom && heldBelowRoom.viewWhoseLengthIsBound && !isUnreachable()) {
            viewWhoseLengthIsBound = builtInFront(heldBelowRoom.viewWhoseLengthIsBound);
            noteKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, viewWhoseLengthIsBound, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), 1, true);
        }
        if (leastRoom && !isUnreachable() && !m_hasFailed) {
            if (index.opcodeOf(heldBelowRoom.relation) == Opcode::LessEq) {
                Draft one(Opcode::Integer, origin);
                one.setInteger(1);
                leastRoom = buildOperatorOnIntegersOfLoop(*this, Opcode::Sub, leastRoom, constant(WTF::move(one)), origin);
            }
            lesserBound = isInteger(leastRoom) ? lesserOf(bound, leastRoom) : Reference();
        }
    }
    {
        SetForScope asked(m_inputHeadAskedAboutTypedCopy, headOfLooksTriedForLists);
        buildTestsOfListsReadInLoop(*this, inputHead, entering, listsRead, guesses.span(), proofs.span(), closedStart, origin);
    }
    bool testsOfListsLeadToClosedStart = false;
    if (sharedCopyIsPlainWalk && !rareWayLeadsToClosedStart) {
        m_code.forEachEdgeInto(closedStart, [&](const Edge&) {
            testsOfListsLeadToClosedStart = true;
        });
    }
    if (m_hasFailed)
        return true;
    if (!closedStart && !asksAtHead && hasTestInFront) {
        fail("a loop is built twice without a test"_s);
        return true;
    }
    noteGuessOpened();
    ++m_numberOfLoopsBuiltTwice;
    BlockID entryOfClosedCopy = sharedEntry;
    Vector<Reference, 8> enteringClosedCopy;
    Vector<Reference, 2> positionsEnteringClosedCopy;
    if (!sharedEntry && closedCopyHasEntry) {
        entryOfClosedCopy = addBlock(otherCopyOpensItsGuesses ? BlockFrequency::Normal : BlockFrequency::Rare);
        for (size_t index = 0; index < entering.size(); ++index) {
            TypeID type = joinTypes(preview.typesOfParameters[index], ownTypeOf(entering[index]));
            enteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
        }
        for (size_t place = 0; place < steppedAtHead.asTheyEntered.size(); ++place) {
            TypeID type = m_types.integer(Range { 0, positionBehindEveryEnd });
            positionsEnteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
        }
        takeFieldsOfScopesIntoClosedCopy(entryOfClosedCopy, preview);
        settleParametersOf(entryOfClosedCopy);
    }
    takeIterationsAsTheyEntered();

    for (ValueID value : readBehindLoop)
        m_frames[frameIndex].listAsBuiltTwice(value);
    m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
    uint32_t namesInFrontOfLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;

    SetForScope ordinalOfCopy(m_ordinalOfCopyOfLoop, m_ordinalOfCopyOfLoop * 16 + 1);
    BlockID typedStart = isUnreachable() ? BlockID() : m_block;
    WalkState stateWhereTestsPassed = m_state;
    if (!sharedEntry) {
        if (closedStart)
            continueIn(closedStart);
        else {
            m_state.clear();
            if (entryOfClosedCopy) {
                closedStart = addBlock(BlockFrequency::Rare);
                m_state = stateWhereTestsPassed;
            }
            m_block = closedStart;
        }
        if (entryOfClosedCopy) {
            if (!isUnreachable()) {
                Vector<Reference, 8> withPositions;
                withPositions.append(entering);
                for (size_t place = 0; place < positionsEnteringClosedCopy.size(); ++place)
                    withPositions.append(steppedAtHead.asTheyEntered[place].second.position);
                EdgeID noEdge;
                if (bringFieldsOfScopesRound(noEdge, entryOfClosedCopy, withPositions))
                    endWithJumpInto(entryOfClosedCopy, withPositions.span());
            }
            for (size_t place = 0; place < positionsEnteringClosedCopy.size(); ++place)
                m_iterationsByIndex[steppedAtHead.asTheyEntered[place].first].position = positionsEnteringClosedCopy[place];
            enterClosedCopy(entryOfClosedCopy, preview);
        }
        if (otherCopyOpensItsGuesses)
            walkCopyOfLoop(inputHead, entryOfClosedCopy ? enteringClosedCopy.span() : entering);
        else {
            ClosedArm closedArm(*this);
            ++m_numberOfCopiesBuiltForSize;
            walkCopyOfLoop(inputHead, entryOfClosedCopy ? enteringClosedCopy.span() : entering);
            --m_numberOfCopiesBuiltForSize;
        }
    }
    forgetValuesOfLoop(inputHead);

    if (typedStart && !m_hasFailed) {
        m_block = typedStart;
        m_state = WTF::move(stateWhereTestsPassed);
        putBackIterationsSteppedAt(steppedAtHead);
        for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
            size_t guess = entered.mayGoByProtocol ? guesses.findIf([&](const GuessInFrontOfLoop& asked) { return asked.value == entered.list; }) : notFound;
            if (guess == notFound || !proofs[guess])
                continue;
            IterationByIndex& iteration = m_iterationsByIndex[place];
            iteration.mayGoByProtocol = false;
            iteration.list = proofs[guess];
            iteration.state = { };
            iteration.iterator = { };
            iteration.next = { };
            iteration.stateHoldsVerdictOnNext = false;
        }
        Vector<std::pair<ValueID, Reference>, 8> boundBefore;
        for (size_t place = 0; place < guesses.size(); ++place) {
            if (!proofs[place])
                continue;
            for (ValueID name : guesses[place].names) {
                boundBefore.append({ name, referenceBoundTo(name) });
                bind(name, proofs[place]);
            }
        }
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.append(WayIntoClosedCopy { frameIndex, inputHead, entryOfClosedCopy, { }, closedStart, { }, ofTypedCopy || (listsRead.areTested && headOfLooksTriedForLists == inputHead) });
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.last().variablesHeld.appendVector(fieldsTestedInFront);
        if (entryOfClosedCopy && lesserBound) {
            m_waysIntoClosedCopies.last().relationThatAsksLesserBound = heldBelowRoom.relation;
            m_waysIntoClosedCopies.last().lesserBound = lesserBound;
            m_waysIntoClosedCopies.last().viewWhoseLengthIsBound = viewWhoseLengthIsBound;
            m_waysIntoClosedCopies.last().accessesBelowRoomOfViews = WTF::move(accessesBelowRoomOfViews);
        }
        for (const CounterTestedInFrontOfLoop& counter : counters) {
            if (entryOfClosedCopy && counter.isHeldWhereItComesRound)
                m_waysIntoClosedCopies.last().countersHeld.append({ counter.position, counter.tested });
        }
        ++m_ordinalOfCopyOfLoop;
        if (typedCopyIsQuietByRow || listsRead.areTested)
            m_effectsSeededForNextRow = effectsOfTypedCopyByRow;
        else if (entryOfClosedCopy && lesserBound && !storesUnderCounterAloneRanOtherCode)
            m_effectsSeededForNextRow = preview.effects;
        else if (entryOfClosedCopy && lesserBound) {
            Effects ofStores = Effects::none();
            ofStores.writes = LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayElements });
            ofStores.merge(m_effectsOutsideArmsOfRow);
            m_effectsSeededForNextRow = ofStores;
        }
        uint32_t guessesOpenedInFront = m_numberOfGuessesOpened;
        m_walkOfLoopWasKept = false;
        m_buildsTestsBesidePlainWalk = false;
        walkCopyOfLoop(inputHead, enteringTypedCopy.span());
        m_typedBuildTestsNothing = !hasTestInFront && variablesCheckedInTurns.isEmpty() && m_numberOfGuessesOpened == guessesOpenedInFront;
        if (m_walkOfLoopWasKept)
            m_reportOfTypedBuildMadeLast = ReportOfTypedBuild { m_effectsOfLoopKeptLast, otherCopyOpensItsGuesses, rareWayLeadsToClosedStart || !testsOfListsLeadToClosedStart };
        if (listsRead.areTested && !m_hasFailed && m_walkOfLoopWasKept && !leavesListsTestedInFrontOfLoopAsTheyAre(m_effectsOfLoopKeptLast))
            m_typedBuildEndedListsTestedInFront = true;
        m_effectsSeededForNextRow = std::nullopt;
        if (holdsCounterBelowRoom && !hasTestInFront && variablesCheckedInTurns.isEmpty() && !ofTypedCopy && !(entryOfClosedCopy && m_waysIntoClosedCopies.last().headAsksLesserBound))
            m_everyTurnOfTypedBuildLeaves = true;
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.removeLast();
        for (auto [name, reference] : boundBefore)
            bind(name, reference);
    }
    takeIterationsAsTheyEntered();

    for (BlockID member : index.membersOf(loop))
        inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
    forgetValuesOfLoop(inputHead);
    m_frames[frameIndex].loopsBeingBuiltTwice.removeLast();
    m_block = { };
    m_state.clear();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
