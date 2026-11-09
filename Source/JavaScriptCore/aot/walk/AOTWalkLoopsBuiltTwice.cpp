/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinPristine.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOperandKinds.h"
#include "AOTSumOverRecords.h"
#include "AOTWalkBirths.h"
#include "AOTWalkBlocksThatEveryTurnPasses.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t largestLoopBuiltTwice = 100;

static bool isTypeOfLink(const TypeArena& types, TypeID held)
{
    Type type = types.at(held);
    return type.hasObjects() && type.hasOnlyLayouts() && !type.hasOnlyClosures() && !type.primitives;
}

static Reference buildTestOfCounterOrLink(Walker& walker, Reference value, TypeID tested, Origin origin)
{
    if (!isTypeOfLink(walker.types(), tested))
        return buildTestOfNumber(walker, value, tested, origin);
    Draft shape(Opcode::ShapeTest, origin);
    shape.setObjectOperand(value);
    shape.setLayouts(walker.types().at(tested).layouts);
    shape.setRefusesMarkedObjects(!walker.letsMarkedObjectsPassTestsOfLinks());
    return walker.build(WTF::move(shape));
}

bool Walker::leavesLoopBeingBuiltTwice(size_t frameIndex, BlockID to) const
{
    if (!m_generatorsWalkedInConsumers.isEmpty() && leavesRegionOfIteration(frameIndex, to))
        return true;
    const Frame& frame = m_frames[frameIndex];
    auto isLeft = [&](LoopID loop) {
        return loop.index() < frame.index->numberOfLoops() && !frame.index->isInLoop(to, loop);
    };
    if (std::ranges::any_of(frame.loopsBeingBuiltTwice, isLeft))
        return true;
    return std::ranges::any_of(m_reversionableLoops, [&](const std::pair<size_t, LoopID>& walked) {
        return walked.first == frameIndex && isLeft(walked.second);
    });
}

void Walker::takeNamesOntoEdge(OpenEdge& edge, size_t frameIndex)
{
    const Frame& frame = m_frames[frameIndex];
    if (!frame.valuesBuiltTwice)
        return;
    edge.names = frame.valuesBuiltTwice->namesOfWay;
    for (ValueID value : frame.valuesBuiltTwice->boundOnWay) {
        if (Reference name = nameBehindStandIn(frameIndex, value, frame.references[value.index()]); name && frame.valuesBuiltTwice->contains(value))
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
            frame.references.placeOf<NamesOfValues::Writer::ForgetsNamesOfWay>(value.index()) = Reference();
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
    size_t numberOfTextsInFront = m_textsInStringBuilders.size();
    m_walkOfLoopBeganTexts = false;
    uint32_t namesAtHead = inputBlock(frameIndex, inputHead).firstNarrowerName;
    Vector<LoopID, 8> touched;
    if (m_frames[frameIndex].loopsWithBlocksTouched.get(loop.index()))
        touched.append(loop);
    while (!touched.isEmpty()) {
        LoopID holder = touched.takeLast();
        forEachBlockAndLoopRightIn(index, holder, [&](BlockID member) {
            if (!m_frames[frameIndex].blocks[member.index()].isAsNew())
                inputBlock(frameIndex, member) = InputBlock { };
        }, [&](LoopID inside) {
            if (m_frames[frameIndex].loopsWithBlocksTouched.get(inside.index()))
                touched.append(inside);
            return true;
        });
        m_frames[frameIndex].loopsWithBlocksTouched.clear(holder.index());
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
    forEachBlockAndLoopRightIn(index, loop, [&](BlockID member) {
        if (m_hasFailed || (index.loopOf(member) != loop && m_frames[frameIndex].blocks[member.index()].firstOpenEdge == noOpenEdge))
            return;
        walkBlock(member);
    }, [&](LoopID inside) {
        if (m_hasFailed || m_frames[frameIndex].blocks[index.loop(inside).head.index()].firstOpenEdge == noOpenEdge)
            return false;
        m_loopThatItsTryHasWalked = { };
        walkBlock(index.loop(inside).head);
        if (m_loopThatItsTryHasWalked == std::pair { frameIndex, inside })
            return true;
        for (BlockID member : index.membersOf(inside).subspan(1)) {
            if (!m_hasFailed && m_frames[frameIndex].blocks[member.index()].firstOpenEdge != noOpenEdge)
                walkBlock(member);
        }
        return true;
    });
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
    m_walkOfLoopBeganTexts = m_textsInStringBuilders.size() > numberOfTextsInFront;
    if (!m_hasFailed)
        endTextsInStringBuildersFrom(numberOfTextsInFront);
    walkCleanupsBesideCopyOfLoop(inputHead);
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
        bool lengthIsFoundByNote = object && object.isValue() && program().nameClassOf(written.key()) == NameClass::Length && findKnown(*this, KnownNote::LengthOfViewIsFoundAbove, object) && loopTakesInstancesBelowViewForItsLength(*this, inputHead, object);
        if (!object || !object.isValue() || lengthIsFoundByNote || isListAsMade(*this, object))
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
    Vector<Reference, 8> held;
    for (uint32_t index = 0; index < head.numberOfParameters(); ++index)
        held.append(referenceOf(head.parameter(index)));
    if (loopCarriesValuesOfConsumerRound(way.inputHead))
        held.appendVector(carriedValuesInFrame(frameIndex));
    BlockID headOfTypedCopy = inputBlock(frameIndex, way.inputHead).head;
    if (!closedCopyTakes(headOfTypedCopy, held.span()))
        return false;

    Reference object = referenceOf(read.objectOperand());
    bool takesInstancesBelow = read.key() == program().keyOf(BuiltinNames::length) && loopTakesInstancesBelowViewForItsLength(*this, way.inputHead, object);
    Reference test = takesInstancesBelow ? buildTestOfViewOrInstanceBelow(*this, object, originOf(read)) : buildTestOfObjectAsMade(*this, object, originOf(read));
    if (!test || m_hasFailed)
        return false;
    BlockID passed = addBlock();
    BlockID failed = addBlock(BlockFrequency::Rare);
    letGuessFailUnasked(failed, originOf(read));
    branch(test, passed, failed);
    continueIn(failed);
    leaveForClosedCopy(headOfTypedCopy, held.span());
    continueIn(passed);
    if (m_hasFailed || isUnreachable())
        return false;
    if (takesInstancesBelow)
        noteThatLengthOfViewIsFoundAbove(*this, object);
    else if (m_types.at(typeOf(object)).hasObjects())
        noteListAsMade(*this, object);
    noteObjectPassedTestOfBeingAsMade(*this, object, test);
    noteGuessOpened();
    return true;
}

std::optional<ListTestedInFrontOfLoop> Walker::findListThatHeadReadsUnderKeyFromInFront(BlockID inputHead, std::span<const ListTestedInFrontOfLoop> lists)
{
    const BodyIndex& index = currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    uint32_t firstValueOfLoop = head.firstParameter().index();
    for (BodyValue written : head.values()) {
        if (written.opcode() == Opcode::LoopHint || written.opcode() == Opcode::Integer)
            continue;
        if (written.opcode() != Opcode::ElemGet)
            return std::nullopt;
        ValueID key = index.valueBehindCasts(written.propertyOperand());
        bool keyStandsInFront = key && (key.index() < firstValueOfLoop || (!index.isParameter(key) && index.opcodeOf(key) == Opcode::Integer));
        Reference named = written.objectOperand().index() < firstValueOfLoop ? referenceOf(written.objectOperand()) : Reference();
        if (!keyStandsInFront || !named || !named.isValue())
            return std::nullopt;
        for (const ListTestedInFrontOfLoop& list : lists) {
            if (list.name != named || list.written)
                continue;
            ListTestedInFrontOfLoop asked = list;
            if (asked.admitsInt32Storage) {
                TypeID seenByLink = facts().linked().resultOfReadByKey(currentFunction(), written.id());
                if (!seenByLink || !m_types.at(m_types.withoutNullish(seenByLink)).isNumberWithoutFraction())
                    return std::nullopt;
                asked.storage = ArrayStorage::Int32;
                asked.admitsInt32Storage = false;
            }
            ValueID listAsWritten = index.valueBehindCasts(written.objectOperand());
            bool turnsStoreIntoIt = std::ranges::any_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
                return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
                    return operation.opcode() == Opcode::ElemSet && index.valueBehindCasts(operation.objectOperand()) == listAsWritten;
                });
            });
            asked.isWritable |= turnsStoreIntoIt;
            return hasArrayStorageForm(asked.storage) ? std::optional(asked) : std::nullopt;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<PassedTest> Walker::askListAtHeadOfTypedCopy(const BodyValue& read)
{
    if (m_waysIntoClosedCopies.isEmpty() || !m_waysIntoClosedCopies.last().listAskedAtHead)
        return std::nullopt;
    const WayIntoClosedCopy& last = m_waysIntoClosedCopies.last();
    struct {
        size_t frameIndex;
        BlockID inputHead;
        Reference listAskedAtHead;
        ArrayStorage storageAskedOfListAtHead;
        bool listAskedAtHeadIsWritable;
    } way { last.frameIndex, last.inputHead, last.listAskedAtHead, last.storageAskedOfListAtHead, last.listAskedAtHeadIsWritable };
    size_t frameIndex = m_frames.size() - 1;
    if (way.frameIndex != frameIndex || m_frames[frameIndex].block != way.inputHead || isUnreachable() || hasTextsInStringBuilders() || !turnOfTypedCopyHasDoneNothing())
        return std::nullopt;
    Reference position = referenceOf(read.propertyOperand());
    if (referenceOf(read.objectOperand()) != way.listAskedAtHead || findListTestedInFrontOfLoop(*this, way.listAskedAtHead) || !position || !m_types.at(typeOf(position)).isInteger())
        return std::nullopt;

    BodyBlock head = currentBodyIndex().block(way.inputHead);
    Vector<Reference, 8> held;
    for (uint32_t index = 0; index < head.numberOfParameters(); ++index)
        held.append(referenceOf(head.parameter(index)));
    if (loopCarriesValuesOfConsumerRound(way.inputHead))
        held.appendVector(carriedValuesInFrame(frameIndex));
    BlockID headOfTypedCopy = inputBlock(frameIndex, way.inputHead).head;
    if (!closedCopyTakes(headOfTypedCopy, held.span()))
        return std::nullopt;

    Origin origin = originOf(read);
    origin.isTailCall = false;
    for (bool asksForKind : { false, true }) {
        Reference test = asksForKind ? askWhetherStorageOfListIsOfKind(*this, way.listAskedAtHead, way.storageAskedOfListAtHead, origin, way.listAskedAtHeadIsWritable) : askWhetherListIsAsMade(*this, way.listAskedAtHead, origin);
        if (!test || m_hasFailed)
            return std::nullopt;
        if (!test.isValue())
            continue;
        BlockID passed = addBlock();
        BlockID failed = addBlock(BlockFrequency::Rare);
        letGuessFailUnasked(failed, origin);
        branch(test, passed, failed);
        continueIn(failed);
        leaveForClosedCopy(headOfTypedCopy, held.span());
        continueIn(passed);
        if (m_hasFailed || isUnreachable())
            return std::nullopt;
    }
    noteKindOfStorageThatTurnFound(*this, way.listAskedAtHead, way.storageAskedOfListAtHead, way.listAskedAtHeadIsWritable);
    noteGuessOpened();
    Draft isPresent(Opcode::IsElementPresent, origin);
    chooseConservativeFields(isPresent);
    isPresent.setArrayOperand(way.listAskedAtHead);
    isPresent.setPositionOperand(position);
    isPresent.setArrayStorage(way.storageAskedOfListAtHead);
    isPresent.setKeyIsIntegral(true);
    Reference isThere = build(WTF::move(isPresent));
    if (!isThere || !isThere.isValue() || m_hasFailed)
        return std::nullopt;
    BlockID found = addBlock();
    BlockID absent = addBlock(BlockFrequency::Rare);
    letGuessFailUnasked(absent, origin);
    branchOnTest(isThere, found, absent);
    continueIn(absent);
    leaveForClosedCopy(headOfTypedCopy, held.span());
    continueIn(found);
    if (m_hasFailed || isUnreachable())
        return std::nullopt;
    count(StatisticID::ListsAskedAtHeadsOfTypedTurns);
    return showElementPresentBehindTest(way.listAskedAtHead, position, findPassedTest(isThere));
}

void Walker::noteHeadOfTurn(BlockID inputHead)
{
    m_state.noteSomethingWasDoneInTurn();
    if (m_waysIntoClosedCopies.isEmpty() || m_numberOfCopiesBuiltForSize || isUnreachable())
        return;
    WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (way.frameIndex != m_frames.size() - 1 || way.inputHead != inputHead)
        return;
    way.isLeftAtOtherCodeOnUsualWay = false;
    way.isLeftAtAccessOnUsualWay = false;
    BodyBlock head = currentBodyIndex().block(inputHead);
    std::span<const ValueID> taken = m_code.at(way.entry).parameters();
    way.heldAtHead.shrink(0);
    way.variablesOfFilesHeldAtHead.shrink(0);
    way.blocksThatPutStoresBack.shrink(0);
    m_statesBehindStoresToPutBack.shrink(0);
    way.headOfTurnInEffectsNet = { };
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
    for (auto [position, held] : way.countersHeld) {
        if (!isTypeOfLink(m_types, held) || position >= head.numberOfParameters())
            continue;
        Reference link = referenceOf(head.parameter(position));
        if (TypeID type = ownTypeOf(link); type && m_types.isSubset(type, held))
            m_state.noteFactsOfCell(link, FactOfCell::BearsNoMark);
    }
    if (m_inputs.options.checksEffects) [[unlikely]]
        m_waysIntoClosedCopies.last().headOfTurnInEffectsNet = build(Draft(Opcode::BeginsWhatMayRunAgain, m_origin));
    if (m_waysIntoClosedCopies.last().looksCountOnTurnsThatBeginAgain)
        holdVariablesOfFilesThatTurnsStore(inputHead);
}

void Walker::holdVariablesOfFilesThatTurnsStore(BlockID inputHead)
{
    const BodyIndex& index = currentBodyIndex();
    bool turnsStoreElements = std::ranges::any_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [](BodyValue operation) {
            return operation.opcode() == Opcode::ElemSet;
        });
    });
    if (!turnsStoreElements)
        return;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::PutModuleVariable)
                continue;
            VariableID variable = operation.variable();
            bool isHeld = m_waysIntoClosedCopies.last().variablesOfFilesHeldAtHead.containsIf([&](const auto& held) {
                return held.first == variable;
            });
            if (isHeld || plan().deferredDefinitions().findDeferred(variable) || plan().variable(variable).function)
                continue;
            Reference held = has(Mechanism::Folding) ? m_state.variable(variable) : Reference();
            if (!held) {
                Draft read(Opcode::GetModuleVariable, m_origin);
                read.setVariable(variable);
                chooseConservativeFields(read);
                if (has(Mechanism::Folding) && m_state.isPastDeadZone(variable))
                    read.setFollowsItsStore(true);
                held = build(WTF::move(read));
                if (m_hasFailed || isUnreachable())
                    return;
                if (held)
                    m_state.noteVariable(variable, held);
            }
            TypeID type = held ? typeOf(held) : TypeID();
            if (type && !(m_types.at(type).primitives & Primitive::Absent))
                m_waysIntoClosedCopies.last().variablesOfFilesHeldAtHead.append({ variable, held });
        }
    }
}

bool Walker::storeIsPutBackWhereTurnBeginsAgain(VariableID variable) const
{
    return mayBeginTurnAgainInClosedCopy() && m_waysIntoClosedCopies.last().variablesOfFilesHeldAtHead.containsIf([&](const auto& held) {
        return held.first == variable;
    });
}

bool Walker::standsInTurnThatLooksCountOnAndCanPutBack() const
{
    return mayBeginTurnAgainInClosedCopy() && m_waysIntoClosedCopies.last().looksCountOnTurnsThatBeginAgain;
}

bool Walker::askLesserBoundAt(const BodyValue& relation)
{
    WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (relation.id() != way.relationThatAsksLesserBound || way.frameIndex != m_frames.size() - 1)
        return false;
    way.headAsksLesserBound = false;
    way.headThatAsksLesserBound = { };
    way.counterBelowRoom.isBelowBound = { };
    way.counterBelowRoom.counter = { };
    if (!way.lesserBound || !m_opensGuesses || m_frames.last().block != way.inputHead || !mayBeginTurnAgainInClosedCopy())
        return false;
    if (way.viewWhoseLengthIsBound && !findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, way.viewWhoseLengthIsBound))
        return false;
    if (way.variableThatIsBound && !takesVariablesThatBoundToBeAsInFront() && !findKnown(*this, KnownNote::VariableIsAsInFrontOfLoop, way.readOfVariableThatIsBound, { }, way.variableThatIsBound.index()))
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
    if (testsEveryPosition() && !isUnreachable() && m_waysIntoClosedCopies.last().variableThatIsBound) {
        FieldOfScope read;
        read.variable = m_waysIntoClosedCopies.last().variableThatIsBound;
        Reference believed = m_waysIntoClosedCopies.last().readOfVariableThatIsBound;
        TypeID type = typeOf(believed);
        if (type && m_types.at(type).isJavaScriptValue() && !(m_types.at(type).primitives & Primitive::Absent)) {
            WalkState atHead = m_state;
            checkThatMemoryAnswersAsBelieved(believed, draftOfReadOf(read));
            m_state = WTF::move(atHead);
        } else
            ++m_numberOfProofsThatNoTestTells;
    }
    if (testsEveryPosition() && !isUnreachable() && m_waysIntoClosedCopies.last().boundBesideLesserBound) {
        Reference number = referenceOf(relation.operand(1));
        if (number && m_types.at(typeOf(number)).isNumber()) {
            WalkState atHead = m_state;
            Draft written(relation.opcode(), originOf(relation));
            chooseConservativeFields(written);
            written.setLeftOperand(counter);
            written.setRightOperand(number);
            Reference asWritten = buildTestOfCheck(WTF::move(written));
            checkThatValuesAreTheSame(asWritten, buildOperatorOnIntegersOfLoop(*this, relation.opcode(), counter, m_waysIntoClosedCopies.last().boundBesideLesserBound, originOf(relation)), originOf(relation));
            m_state = WTF::move(atHead);
        } else
            ++m_numberOfProofsThatNoTestTells;
    }
    Reference test = buildOperatorOnIntegersOfLoop(*this, relation.opcode(), counter, m_waysIntoClosedCopies.last().lesserBound, originOf(relation));
    if (!test || !test.isValue() || isUnreachable())
        return false;
    bind(relation.id(), test);
    m_waysIntoClosedCopies.last().headAsksLesserBound = true;
    m_waysIntoClosedCopies.last().headThatAsksLesserBound = inputBlock(m_frames.size() - 1, m_waysIntoClosedCopies.last().inputHead).head;
    if (m_waysIntoClosedCopies.last().variableThatIsBound)
        count(StatisticID::HeadsThatAskBoundInVariable);
    m_waysIntoClosedCopies.last().counterBelowRoom.isBelowBound = test.id();
    claimThatCounterNeverLiesBelowItsStart();
    noteGuessOpened();
    if (m_waysIntoClosedCopies.last().counterBelowRoom.start)
        count(StatisticID::CountersHeldBelowRoomOfViews);
    return true;
}

void Walker::claimThatCounterNeverLiesBelowItsStart()
{
    WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    WayIntoClosedCopy::CounterBelowRoomOfViews& held = way.counterBelowRoom;
    BlockID head = inputBlock(way.frameIndex, way.inputHead).head;
    if (!held.start || !head || m_triesOfLoops.isEmpty() || m_openLoops.isEmpty() || m_openLoops.last().head != head)
        return;
    const TryOfLoop& tried = m_triesOfLoops.last();
    std::span<const ValueID> entering = m_openLoops.last().recordsEntering();
    if (tried.frameIndex != way.frameIndex || tried.inputHead != way.inputHead || m_answersOfLoops[tried.answer].counterWasSteppedDown || held.position >= entering.size() || held.position >= m_code.at(head).parameters().size())
        return;
    if (!everyWayStepsUpFrom(m_code, m_types, entering[held.position], held.start, { }, scopedLambda<bool(ValueID)>([](ValueID) { return false; })))
        return;
    held.counter = m_code.at(head).parameters()[held.position];
    noteKnown(*this, KnownNote::NeverLiesBelow, Reference::value(held.counter), referenceTo(held.start), 0, LocationMasks(), 1);
}

bool Walker::counterComesRoundNotBelowItself(BlockID head, std::span<const Reference> arguments)
{
    for (const WayIntoClosedCopy& way : m_waysIntoClosedCopies) {
        const WayIntoClosedCopy::CounterBelowRoomOfViews& held = way.counterBelowRoom;
        if (!held.counter || m_code.blockOf(held.counter) != head)
            continue;
        bool stepHolds = !held.stepIsFromZeroUp || findPassedTest(referenceTo(held.stepIsFromZeroUp));
        if (stepHolds && held.position < arguments.size() && everyWayStepsUpFrom(m_code, m_types, arguments[held.position].id(), held.counter, held.stepIsFromZeroUp, scopedLambda<bool(ValueID)>([&](ValueID each) {
            return m_code.at(each).opcode != Opcode::Parameter || !m_code.at(m_code.blockOf(each)).isLoopHead;
        })))
            continue;
        for (size_t place = m_triesOfLoops.size(); place--;) {
            TryOfLoop& tried = m_triesOfLoops[place];
            if (tried.frameIndex >= m_frames.size() || inputBlock(tried.frameIndex, tried.inputHead).head != head)
                continue;
            tried.misfits = true;
            m_answersOfLoops[tried.answer].counterWasSteppedDown = true;
            return false;
        }
        fail("a counter comes round below itself outside a try"_s);
        return false;
    }
    return true;
}

std::optional<PassedTest> Walker::findWhatHoldsPositionBelowRoomOfView(Reference view, Reference position)
{
    TypeID type = view && view.isRecord() && position && position.isRecord() ? typeOf(position) : TypeID();
    if (!type || !m_types.at(type).isInteger())
        return std::nullopt;
    for (size_t place = m_waysIntoClosedCopies.size(); place--;) {
        const WayIntoClosedCopy::CounterBelowRoomOfViews& held = m_waysIntoClosedCopies[place].counterBelowRoom;
        std::optional<uint64_t> length = held.isBelowBound ? findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view) : std::nullopt;
        if (!length)
            continue;
        GroundsOfPosition grounds { .view = view.id(), .position = position.id(), .length = Reference::fromBits(static_cast<uint32_t>(*length)).id(), .isBelow = held.isBelowBound, .stepIsFromZeroUp = held.stepIsFromZeroUp };
        if (held.counter && findKnown(*this, KnownNote::NeverLiesBelow, Reference::value(held.counter), referenceTo(held.start))) {
            grounds.counter = held.counter;
            grounds.start = held.start;
        }
        std::optional<PassedTest> shown = showPositionInside(grounds);
        for (size_t asked = 0; !shown && asked < held.fromZeroUp.size(); ++asked) {
            grounds.isFromZeroUp = held.fromZeroUp[asked];
            shown = showPositionInside(grounds);
        }
        if (shown)
            return shown;
    }
    return std::nullopt;
}

std::optional<PassedTest> Walker::showPositionInside(const GroundsOfPosition& grounds)
{
    bool isShown = groundsShowPositionInside(m_code, m_types, grounds.position, grounds, scopedLambda<bool(ValueID, bool)>([&](ValueID relation, bool answersYes) {
        return !!findPassedTest(referenceTo(relation), answersYes);
    }));
    if (!isShown)
        return std::nullopt;
    PassedTest passed;
    auto restOn = [&](ValueID relation, bool answersYes) {
        if (std::optional<PassedTest> one = relation ? findPassedTest(referenceTo(relation), answersYes) : std::nullopt)
            passed = passed.with(*one);
    };
    restOn(grounds.isBelow, grounds.isBelowAnswersYes);
    restOn(grounds.isFromZeroUp, true);
    restOn(grounds.stepIsFromZeroUp, true);
    restOn(grounds.lengthHoldsBound, true);
    passed.m_groundsOfPosition = grounds;
    return passed;
}

std::optional<PassedTest> Walker::showPositionInsideBehindTests(Reference view, Reference position, Reference length, const PassedTest& fromZeroUp, const PassedTest& below)
{
    if (!view || !view.isRecord() || !position || !position.isRecord() || !length || !length.isRecord())
        return std::nullopt;
    GroundsOfPosition grounds { .view = view.id(), .position = position.id(), .length = length.id() };
    if (!below.sides().empty()) {
        grounds.isBelow = below.sides().front().test;
        grounds.isBelowAnswersYes = below.sides().front().answersYes;
    }
    if (!fromZeroUp.sides().empty() && fromZeroUp.sides().front().answersYes)
        grounds.isFromZeroUp = fromZeroUp.sides().front().test;
    return showPositionInside(grounds);
}

Walker::NumberAskedBesideNumber Walker::askValueBesideNumberForNumber(const BodyValue& operation)
{
    if (effectRuleOf(operation.opcode()) != EffectRule::Operator || numberOfNamedOperandsOf(operation.opcode()) != 2 || !m_opensGuesses || !standsInTurnThatLooksCountOn() || !mayBeginTurnAgainInClosedCopy())
        return { };
    if (!otherCopyOfTypedCopyOpensItsGuesses() || program().at(currentFunction()).has(FunctionFlags::IsChecked) || leavesOutQuestionThatEveryTurnWouldAsk())
        return { };
    static constexpr OpcodeSet operatorsAskedOnBothSides = allRelations | OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod };
    Reference left = referenceOf(operation.operand(0));
    Reference right = referenceOf(operation.operand(1));
    if (operatorsAskedOnBothSides.contains(operation.opcode()) && left && left.isValue() && right && right.isValue() && left != right && m_types.saysNothing(typeOf(left)) && m_types.saysNothing(typeOf(right))) {
        proveExpectedTypeOrBeginTurnAgain(*this, operation.operand(0), TypeTable::number, draftOf(operation).origin(), TurnsThatAsk::SomeBesideCopyWithoutGuess);
        if (isUnreachable() || m_hasFailed)
            return { };
    }
    for (uint32_t asked = 0; asked < 2; ++asked) {
        Reference value = referenceOf(operation.operand(asked));
        Reference beside = referenceOf(operation.operand(1 - asked));
        if (!value || !value.isValue() || !beside || !m_types.saysNothing(typeOf(value)) || !m_types.at(typeOf(beside)).isNumber())
            continue;
        proveExpectedTypeOrBeginTurnAgain(*this, operation.operand(asked), TypeTable::number, draftOf(operation).origin(), TurnsThatAsk::SomeBesideCopyWithoutGuess);
        if (isUnreachable() || m_hasFailed)
            return { };
        Reference proven = value;
        while (Reference checked = m_state.checkedNameOf(proven))
            proven = checked;
        if (proven == value)
            return { };
        count(StatisticID::OperandsAskedForNumberInQuietTurns);
        return { asked, proven };
    }
    return { };
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
        if (!guessAtCalleeAsksForBuiltinWhereItFails(*this, operation, operand) && (operand != operation.operand(0) || !rowIsToldApartFromViewsInTurn(*this, operation, expected)))
            wanted.append({ operand, expected });
    });
    for (auto [operand, expected] : wanted) {
        if (mayBeginTurnAgainInClosedCopy())
            proveExpectedTypeOrBeginTurnAgain(*this, operand, expected, draftOf(operation).origin());
    }
}

bool Walker::turnsOfTypedCopyMayBeginAgain(BlockID inputHead, const LoopPreview& preview)
{
    size_t frameIndex = m_frames.size() - 1;
    if ((!preview.effects.mayRunOtherCode() && !pushOfLoopFillsLiteral(inputHead)) || hasTextsInStringBuilders() || m_numberOfCopiesBuiltForSize || (isUnderHandler() && (isUnderHandlerOtherThanCleanups() || !loopReckonsWithWhatItReads(inputHead)))
        || (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !iterationsSteppedAtHandOverPositionsAlone(inputHead)))
        return false;
    return currentBodyIndex().loop(currentBodyIndex().loopOf(inputHead)).numberOfLoopsWithin <= 1;
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

TypeID Walker::typeAskedOfWhatOtherCodeBringsRound(BlockID head, TypeID ofParameter, TypeID comingRound)
{
    if (m_waysIntoClosedCopies.isEmpty())
        return { };
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (!way.turnsThatRunOtherCodeComeRound || way.frameIndex != m_frames.size() - 1 || inputBlock(way.frameIndex, way.inputHead).head != head)
        return { };
    if (m_types.isSubset(comingRound, TypeTable::number) || !m_types.isSubset(ofParameter, TypeTable::number))
        return { };
    if (m_types.at(ofParameter).isInteger() && m_types.overlap(comingRound, TypeTable::int32))
        return TypeTable::int32;
    return m_types.overlap(comingRound, TypeTable::number) ? TypeID(TypeTable::number) : TypeID();
}

bool Walker::entryTakes(BlockID entry, std::span<const Reference> besideFieldsOfScopes) const
{
    std::span<const ValueID> taken = m_code.at(entry).parameters();
    if (taken.size() - numberOfFieldsOfScopesThatBlockTakes(entry) != besideFieldsOfScopes.size())
        return false;
    for (size_t index = 0; index < besideFieldsOfScopes.size(); ++index) {
        TypeID type = ownTypeOf(besideFieldsOfScopes[index]);
        if (!type || !m_types.isSubset(type, m_code.at(taken[index]).type))
            return false;
    }
    return true;
}

void Walker::enterWithFieldsOfScopes(BlockID entry, std::span<const Reference> besideFieldsOfScopes)
{
    if (!entryTakes(entry, besideFieldsOfScopes)) {
        fail("the entry of a copy of a loop does not take what is handed to it"_s);
        return;
    }
    Vector<Reference, 8> handedOn(besideFieldsOfScopes);
    EdgeID noEdge;
    if (!bringFieldsOfScopesRound(noEdge, entry, handedOn))
        return;
    if (m_block && m_code.at(entry).frequency == BlockFrequency::Unlikely)
        m_code.makeBlockUnlikely(m_block, true);
    crossEdge({ }, entry, handedOn.span());
}

bool Walker::linkComesRoundThatMayBearMark(BlockID head, uint32_t parameter, Reference comesRound)
{
    if (m_waysIntoClosedCopies.isEmpty())
        return false;
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (inputBlock(way.frameIndex, way.inputHead).head != head)
        return false;
    TypeID held = typeThatCounterIsHeldTo(way.inputHead, parameter);
    return held && isTypeOfLink(m_types, held) && m_types.isSubset(m_code.at(m_code.at(head).parameters()[parameter]).type, held) && !m_state.factsOfCell(comesRound).contains(FactOfCell::BearsNoMark);
}

bool Walker::closedCopyTakes(BlockID head, std::span<const Reference> besideFieldsOfScopes)
{
    if (m_waysIntoClosedCopies.isEmpty())
        return false;
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (inputBlock(way.frameIndex, way.inputHead).head != head)
        return false;
    if (m_code.at(head).parameters().size() - numberOfFieldsOfScopesThatBlockTakes(head) != besideFieldsOfScopes.size())
        return false;
    return entryTakes(way.entry, besideFieldsOfScopes);
}

void Walker::leaveForClosedCopy(BlockID head, std::span<const Reference> besideFieldsOfScopes)
{
    if (isUnreachable())
        return;
    if (!head || m_waysIntoClosedCopies.isEmpty() || inputBlock(m_waysIntoClosedCopies.last().frameIndex, m_waysIntoClosedCopies.last().inputHead).head != head) {
        fail("a turn leaves for the closed copy of another loop"_s);
        return;
    }
    if (m_code.at(head).parameters().size() - numberOfFieldsOfScopesThatBlockTakes(head) != besideFieldsOfScopes.size()) {
        fail("a turn leaves for a closed copy without all that its head holds"_s);
        return;
    }
    enterWithFieldsOfScopes(m_waysIntoClosedCopies.last().entry, besideFieldsOfScopes);
}

Reference Walker::askAgainWhereItComesRound(EdgeID& edge, BlockID head, TypeID heldTo, std::span<const Reference> besideFieldsOfScopes, const ScopedLambda<Reference()>& whatComesRound)
{
    if (!heldTo || !closedCopyTakes(head, besideFieldsOfScopes))
        return { };
    if (edge)
        layBlockOnEdge(edge, head);
    Reference comesRound = whatComesRound();
    bool isLink = m_waysIntoClosedCopies.last().countersHeld.containsIf([&](const auto& counter) { return counter.second == heldTo; });
    Reference test = !comesRound ? Reference() : isLink ? buildTestOfCounterOrLink(*this, comesRound, heldTo, m_origin) : buildTestOfNumber(*this, comesRound, heldTo, m_origin);
    if (!test || !test.isValue())
        return { };
    BlockID passed = addBlock();
    BlockID failed = addBlock(BlockFrequency::Rare);
    letGuessFailUnasked(failed, m_origin);
    branch(test, passed, failed);
    continueIn(failed);
    leaveForClosedCopy(head, besideFieldsOfScopes);
    continueIn(passed);
    return isUnreachable() ? Reference() : buildProofOfNumber(*this, comesRound, test, m_origin);
}

bool Walker::turnOfTypedCopyHasDoneNothing() const
{
    return !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().isEntered && !isUnreachable() && m_state.nothingWasDoneInTurn(m_waysIntoClosedCopies.last().numberOfTurn) && !handlerKeepsTurnsFromBeginningAgain() && !hasTextsInStringBuilders();
}

bool Walker::mayBeginTurnAgainInClosedCopy() const
{
    return !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().isEntered && !isUnreachable() && m_state.allThatWasDoneInTurnCanBePutBack(m_waysIntoClosedCopies.last().numberOfTurn) && !handlerKeepsTurnsFromBeginningAgain() && !hasTextsInStringBuilders();
}

bool Walker::noOtherCodeRanInTurnThoughCopiesTookItsMark(uint32_t numberOfTurn) const
{
    uint32_t mark = m_state.markOfTurn();
    for (size_t index = m_frames.size(); index-- && mark && WalkState::turnOfMark(mark) != numberOfTurn;) {
        const Frame& frame = m_frames[index];
        if (frame.markOfTurnThatCopyTook && WalkState::turnOfMark(mark) == frame.numberOfCopy)
            mark = frame.markOfTurnThatCopyTook;
    }
    return numberOfTurn && WalkState::turnOfMark(mark) == numberOfTurn;
}

std::optional<uint32_t> Walker::openLoopOfTypedCopy(TurnOfTypedCopy turn) const
{
    bool standsInTurn = turn == TurnOfTypedCopy::HasDoneNothing ? turnOfTypedCopyHasDoneNothing()
        : !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().isEntered && !isUnreachable() && (turn == TurnOfTypedCopy::HasRunNoOtherCode ? m_state.noOtherCodeRanInTurn(m_waysIntoClosedCopies.last().numberOfTurn) : noOtherCodeRanInTurnThoughCopiesTookItsMark(m_waysIntoClosedCopies.last().numberOfTurn)) && !isUnderHandler() && !hasTextsInStringBuilders();
    if (!standsInTurn || !has(Mechanism::LoopInvariants))
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

bool Walker::standsInFrontOfTypedCopy(Reference reference, TurnOfTypedCopy turn) const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy(turn);
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

bool Walker::turnsOfTypedCopyWriteNoMemberGuards() const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy();
    if (!loop)
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(declaredBoundInTable(Opcode::AreMembersUnchanged).reads);
}

bool Walker::everyTurnOfTypedCopyFindsTheSameUnderName(PropertyKeyID key) const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy(TurnOfTypedCopy::HasRunNoOtherCode);
    if (!loop)
        return false;
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (!m_frames[way.frameIndex].index->dominates(m_frames[way.frameIndex].block, way.lastBlockThatEveryTurnPasses) || !executesOnEveryIteration(*loop, currentHoistPoint()))
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    LocationMasks found(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks });
    found.add(program(), MemoryLocation::objectField(key));
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(found);
}

bool Walker::everyTurnOfTypedCopyFindsTheSameUnderEveryName() const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy(TurnOfTypedCopy::HasRunNoOtherCode);
    if (!loop)
        return false;
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (!m_frames[way.frameIndex].index->dominates(m_frames[way.frameIndex].block, way.lastBlockThatEveryTurnPasses) || !executesOnEveryIteration(*loop, currentHoistPoint()))
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(declaredBoundInTable(Opcode::GetOwnValueUnderKeyOrAbsent).reads);
}

Walker::ObjectsMadeHereThatLoopMentions Walker::objectsMadeHereThatLoopMentions(BlockID inputHead)
{
    ObjectsMadeHereThatLoopMentions mentioned;
    const BodyIndex& index = currentBodyIndex();
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    auto mention = [&](ValueID value) {
        if (!value || value.index() >= firstValue)
            return;
        Reference reference = referenceOf(value);
        if (!reference || !reference.isValue())
            return;
        ValueID made = rootOfCellInSweptCode(m_code, reference.id());
        if (!m_code.isConstant(made) && makesNewCell(m_code.at(made)) && (mentioned.isEmpty() || mentioned.last() != made.index()))
            mentioned.append(made.index());
    };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values())
            operation.forEachOperand([&](ValueID operand, uint32_t) { mention(operand); });
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, auto...) { mention(operand); });
        for (BodyTarget target : end.targets()) {
            for (ValueID argument : target.arguments())
                mention(argument);
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                mention(argument);
        }
    }
    std::ranges::sort(mentioned);
    mentioned.shrink(std::ranges::unique(mentioned).begin() - mentioned.begin());
    return mentioned;
}

std::optional<Walker::IsItselfMentioned> Walker::isOtherThanRestOf(const ObjectsMadeHereThatLoopMentions& mentioned, Reference other) const
{
    if (!other.isValue())
        return other.isConstant() ? std::optional(IsItselfMentioned::No) : std::nullopt;
    ValueID root = rootOfCellInSweptCode(m_code, other.id());
    if (!m_code.isConstant(root) && makesNewCell(m_code.at(root)))
        return std::ranges::binary_search(mentioned, root.index()) ? IsItselfMentioned::Yes : IsItselfMentioned::No;
    if (m_inputs.options.takesValuesThatComeLaterForOthersThanWhatIsMadeHere) [[unlikely]]
        return IsItselfMentioned::No;
    return mentioned.isEmpty() || root.index() < mentioned.first() ? std::optional(IsItselfMentioned::No) : std::nullopt;
}

void Walker::hearEffectsOfWriterInTries(const Effects& effects, const std::optional<Vector<uint32_t, 2>>& objectsMadeHere)
{
    if (!objectsMadeHere || m_triesOfLoops.isEmpty()) {
        hearEffectsInTries(effects);
        return;
    }
    Effects heard = effects;
    heard.writesObjectsMadeHereAlone = true;
    TryOfLoop& innermost = m_triesOfLoops.last();
    for (uint32_t made : *objectsMadeHere) {
        if (made < innermost.numberOfRecordsAtHead && (innermost.objectsMadeInFrontThatAreWritten.isEmpty() || innermost.objectsMadeInFrontThatAreWritten.last() != made))
            innermost.objectsMadeInFrontThatAreWritten.append(made);
    }
    hearEffectsInTries(heard);
}

bool Walker::turnsOfTypedCopyLeaveListsAsTheyAre(Reference list, TurnOfTypedCopy turn)
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy(turn);
    if (!loop)
        return false;
    if (m_triesOfLoops.isEmpty() || !m_triesOfLoops.last().othersAreSparedAtHead)
        return leavesListsTestedInFrontOfLoopAsTheyAre(*this, m_openLoops[*loop].effects);
    const TryOfLoop& tried = m_triesOfLoops.last();
    std::optional<IsItselfMentioned> other = isOtherThanRestOf(*tried.othersAreSparedAtHead, list);
    if (!other || (*other == IsItselfMentioned::Yes && !tried.listsMadeHereThatAreSparedAtHead.contains(rootOfCellInSweptCode(m_code, list.id()).index())))
        return leavesListsTestedInFrontOfLoopAsTheyAre(*this, m_openLoops[*loop].effects);
    ListTestedInFrontOfLoop asked { list, ArrayStorage::Other, false };
    return leavesListsTestedInFrontOfLoopAsTheyAre(*this, m_openLoops[*loop].effects, { &asked, 1 }, *tried.othersAreSparedAtHead);
}

void Walker::noteLengthOfViewAskedForWrittenPlace(Reference view)
{
    if (m_triesOfLoops.isEmpty() || m_openLoops.isEmpty() || !view.isValue() || !standsInFrontOfLoop(m_openLoops.size() - 1, view) || leavesOutQuestionThatEveryTurnWouldAsk(QuestionIsAsked::Anywhere))
        return;
    m_triesOfLoops.last().asksLengthsOfViewsFromInFrontForWrittenPlaces = true;
}

void Walker::noteListFromInFrontTakenApart(Reference list)
{
    if (m_triesOfLoops.isEmpty() || m_openLoops.isEmpty() || !list.isValue() || !standsInFrontOfLoop(m_openLoops.size() - 1, list) || leavesOutQuestionThatEveryTurnWouldAsk(QuestionIsAsked::Anywhere))
        return;
    m_triesOfLoops.last().takesApartListsFromInFront = true;
}

bool Walker::turnsOfTypedCopyLeaveViewsAsTheyAre(TurnOfTypedCopy turn) const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy(turn);
    if (!loop)
        return false;
    const Effects& ofTurns = m_openLoops[*loop].effects;
    return !ofTurns.mayRunOtherCode() && !ofTurns.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }));
}

bool Walker::turnsOfTypedCopyStoreOnlyRawDoublesIntoLists(TurnOfTypedCopy turn) const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy(turn);
    return loop && !(m_openLoops[*loop].effects.writes.maskOf(AbstractHeap::Elements) & markOfElementsOfArraysNotStoredAsRawDoubles);
}

bool Walker::turnsOfTypedCopyStoreIntoNoList() const
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy();
    return loop && !m_openLoops[*loop].effects.writes.maskOf(AbstractHeap::Elements);
}

bool Walker::isBuiltInTurnThatMayBeginAgain(Reference reference) const
{
    if (!reference || !reference.isValue())
        return false;
    if (std::optional<uint32_t> loop = openLoopOfTypedCopy())
        return !standsInFrontOfLoop(*loop, reference);
    if (makesNewCell(m_code.at(reference.id())) && !m_waysIntoClosedCopies.isEmpty()) {
        const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
        for (uint32_t place = m_openLoops.size(); place--;) {
            const OpenLoop& loop = m_openLoops[place];
            if (loop.frame == way.frameIndex && m_frames[loop.frame].index->loop(loop.loop).head == way.inputHead)
                return !standsInFrontOfLoop(place, reference);
        }
    }
    return false;
}

bool Walker::leavesOutQuestionThatEveryTurnWouldAsk(QuestionIsAsked isAsked) const
{
    if (!m_walksCopyWithoutQuestionsOfEveryTurn || m_waysIntoClosedCopies.isEmpty())
        return false;
    std::optional<uint32_t> place = innermostOpenLoop();
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (isAsked == QuestionIsAsked::InCopiedCalleesAlone && m_frames.size() - 1 <= way.frameIndex)
        return false;
    if (!place || m_openLoops[*place].frame != way.frameIndex || m_frames[way.frameIndex].index->loop(m_openLoops[*place].loop).head != way.inputHead)
        return true;
    HoistPoint placeInText = currentHoistPoint();
    placeInText.block = { };
    return m_frames[way.frameIndex].index->dominates(m_frames[way.frameIndex].block, way.lastBlockThatEveryTurnPasses) && executesOnEveryIteration(*place, placeInText);
}

bool Walker::branchInFrontOfTypedCopy(Reference test, bool passesWithYes, TurnOfTypedCopy turn, TurnsThatAsk turnsThatAsk)
{
    std::optional<uint32_t> place = openLoopOfTypedCopy(turn);
    if (!place || !test || !test.isValue() || !standsInFrontOfLoop(*place, test) || representationOf(test) != Representation::Boolean)
        return false;
    WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    HoistPoint placeInText = currentHoistPoint();
    placeInText.block = { };
    bool everyTurnPasses = m_frames[way.frameIndex].index->dominates(m_frames[way.frameIndex].block, way.lastBlockThatEveryTurnPasses) && executesOnEveryIteration(*place, placeInText);
    bool someTurnsAloneAskBesideCopy = turnsThatAsk != TurnsThatAsk::All && !everyTurnPasses && way.startOfCopyWithoutGuessesOfSomeTurns;
    bool everyTurnAsksForNumberBesideCopy = turnsThatAsk != TurnsThatAsk::All && everyTurnPasses && way.startOfCopyWithoutGuessesOfSomeTurns && m_frames.size() - 1 > way.frameIndex && m_code.at(test.id()).opcode == Opcode::TypeTest && m_types.at(m_code.at(test.id()).testedType()).isNumber();
    bool everyTurnAsksForLengthOfViewBesideCopy = turnsThatAsk != TurnsThatAsk::All && everyTurnPasses && way.startOfCopyWithoutGuessesOfSomeTurns && m_code.at(test.id()).opcode == Opcode::Less
        && m_code.at(m_code.valueBehindNames<opcodesThatGiveSameValue>(m_code.at(test.id()).operand(OperandIndex::Less::right))).opcode == Opcode::GetTypedArrayLength;
    bool failingWayLeadsToCopyBeside = someTurnsAloneAskBesideCopy || everyTurnAsksForNumberBesideCopy || everyTurnAsksForLengthOfViewBesideCopy;
    if (turnsThatAsk == TurnsThatAsk::SomeBesideCopyWithoutGuess && !everyTurnPasses && !someTurnsAloneAskBesideCopy)
        return false;
    if ((turnsThatAsk == TurnsThatAsk::All || m_code.at(m_block).frequency != BlockFrequency::Normal) && !everyTurnPasses)
        return false;
    if (m_openLoops[*place].blocksOfFrontOfQuietTurns.contains(m_code.blockOf(test.id())))
        return !someTurnsAloneAskBesideCopy && branchInFrontOfQuietTurns(*place, test, passesWithYes);
    if (failingWayLeadsToCopyBeside)
        way.guessOfSomeTurnsIsAskedInFront = true;
    if (everyTurnAsksForNumberBesideCopy || everyTurnAsksForLengthOfViewBesideCopy)
        way.questionOfEveryTurnIsAskedInFront = true;
    OpenLoop& loop = m_openLoops.changeable(*place);
    BlockID front = loop.blockInFront;
    BlockID next = m_code.addBlock(BlockRole::Ordinary, m_code.at(front).frequency);
    m_code.setLoopMarks(next, m_code.at(front).isInLoop, false);
    BlockID failing = failingWayLeadsToCopyBeside ? way.startOfCopyWithoutGuessesOfSomeTurns : way.startFromInFront;

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

std::optional<PassedTest> Walker::goOnInTypedCopyWhereTestInFrontPasses(Reference test, bool passesWithYes, TurnOfTypedCopy turn)
{
    if (!branchInFrontOfTypedCopy(test, passesWithYes, turn, TurnsThatAsk::SomeBesideCopyWithoutGuess))
        return std::nullopt;
    return PassedTest(test.id(), passesWithYes);
}

Walker::InFrontOfTypedCopy::InFrontOfTypedCopy(Walker& walker, bool isInFront, TurnOfTypedCopy turn)
    : m_walker(walker)
    , m_isInFront(isInFront)
{
    std::optional<uint32_t> loop = m_isInFront ? walker.openLoopOfTypedCopy(turn) : std::nullopt;
    m_isInFront = !!loop;
    if (!m_isInFront)
        return;
    m_blockInside = std::exchange(walker.m_block, walker.m_openLoops[*loop].blockInFront);
    m_numberOfLoopsThatMightBeLeft = std::exchange(walker.m_numberOfLoopsThatMayBeLeft, *loop);
}

Walker::InFrontOfTypedCopy::~InFrontOfTypedCopy()
{
    if (!m_isInFront)
        return;
    m_walker.m_block = m_walker.m_hasFailed ? BlockID() : m_blockInside;
    m_walker.m_numberOfLoopsThatMayBeLeft = m_numberOfLoopsThatMightBeLeft;
}

bool Walker::someOperationOfRowThatRanOtherCodeIsCall(const BodyIndex& index) const
{
    return std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
        return ran && effectRuleOf(index.value(ran).opcode()) == EffectRule::Call;
    });
}

bool Walker::looksCountOnTurnsThatBeginAgain(BlockID inputHead)
{
    if (m_numberOfCopiesBuiltForSize || !m_opensGuesses || !has(Mechanism::Guesses) || (isUnderHandler() && (isUnderHandlerOtherThanCleanups() || !loopReckonsWithWhatItReads(inputHead))) || hasTextsInStringBuilders())
        return false;
    if (m_inputHeadAskedAboutTypedCopy == inputHead)
        return true;
    return !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().looksCountOnTurnsThatBeginAgain && m_waysIntoClosedCopies.last().frameIndex == m_frames.size() - 1 && m_waysIntoClosedCopies.last().inputHead == inputHead;
}

bool Walker::textNamesHolderOfCopyAtWork(ValueID written) const
{
    if (m_holderThatIsNoBuiltin.frameIndex != m_frames.size() - 1 || !written)
        return false;
    ValueID behindCasts = currentBodyIndex().valueBehindCasts(written);
    if (std::ranges::contains(m_holderThatIsNoBuiltin.names, written) || std::ranges::contains(m_holderThatIsNoBuiltin.names, behindCasts))
        return true;
    const BodyIndex& index = currentBodyIndex();
    if (ValueID variable = m_holderThatIsNoBuiltin.readOfVariableInLoop; variable && behindCasts && !index.isParameter(behindCasts)) {
        BodyValue read = index.value(behindCasts);
        if (ValueID checked = read.opcode() == Opcode::CheckInitialized ? index.valueBehindCasts(read.operand(0)) : ValueID(); checked && !index.isParameter(checked))
            read = index.value(checked);
        BodyValue ofHolder = index.value(variable);
        if (read.opcode() == ofHolder.opcode())
            return ofHolder.opcode() == Opcode::GetModuleVariable ? read.variable() == ofHolder.variable() : read.environmentOperand() == ofHolder.environmentOperand() && read.layout() == ofHolder.layout() && read.field() == ofHolder.field();
    }
    return isHolderOfCopyAtWork(referenceBoundTo(behindCasts ? behindCasts : written));
}

bool Walker::isHolderOfCopyAtWork(Reference value) const
{
    if (!value || !value.isValue())
        return false;
    ValueID root = rootOfCellInSweptCode(m_code, value.id());
    if (m_holderThatIsNoBuiltin.value && m_holderThatIsNoBuiltin.value.isValue()) {
        ValueID rootOfHolder = rootOfCellInSweptCode(m_code, m_holderThatIsNoBuiltin.value.id());
        if (rootOfHolder == root)
            return true;
        if (m_holderThatIsNoBuiltin.readOfVariableInLoop && !m_code.isConstant(root) && !m_code.isConstant(rootOfHolder) && m_code.at(root).opcode == Opcode::GetModuleVariable && m_code.at(rootOfHolder).opcode == Opcode::GetModuleVariable && m_code.at(root).variable() == m_code.at(rootOfHolder).variable())
            return true;
    }
    if (m_holderThatIsNoBuiltin.frameIndex >= m_frames.size())
        return false;
    return std::ranges::any_of(m_holderThatIsNoBuiltin.names, [&](ValueID name) {
        Reference bound = referenceBoundIn(m_frames[m_holderThatIsNoBuiltin.frameIndex], name);
        return bound && bound.isValue() && rootOfCellInSweptCode(m_code, bound.id()) == root;
    });
}

bool Walker::standsInTurnThatLooksCountOn() const
{
    if (m_looksTakeFormsBehindLooksAsBuilt)
        return true;
    return turnOfTypedCopyHasDoneNothing() && m_waysIntoClosedCopies.last().looksCountOnTurnsThatBeginAgain;
}

bool Walker::stretchMayRunAsWrittenInstead() const
{
    const StretchWithArmsAsWritten& stretch = m_stretchWithArmsAsWritten;
    return stretch.asWritten && stretch.numberOfFrames == m_frames.size() && !isUnreachable() && m_state.nothingWasDoneInTurn(stretch.number);
}

void Walker::runStretchAsWrittenInstead()
{
    jumpTo(m_stretchWithArmsAsWritten.asWritten);
}

uint32_t Walker::beginStretchInWhichNothingWasDone()
{
    uint32_t number = ++m_numberOfTypedCopiesEntered;
    m_state.noteHeadOfTurn(number);
    return number;
}

bool Walker::storeIntoElementCanBePutBack() const
{
    return !m_isPuttingStoresBack && mayBeginTurnAgainInClosedCopy() && m_code.at(m_block).frequency == BlockFrequency::Normal;
}

void Walker::noteStoreBeingBuiltToPutBack(StoreToPutBack store)
{
    if (m_storesToPutBack.isEmpty())
        m_storesToPutBack.append(StoreToPutBack { });
    store.previous = m_state.newestStoreToPutBack();
    store.numberOfTurn = m_waysIntoClosedCopies.last().numberOfTurn;
    m_storesToPutBack.append(store);
    m_storeBeingBuiltToPutBack = m_storesToPutBack.size() - 1;
}

void Walker::dropWayIntoClosedCopy()
{
    m_waysIntoClosedCopies.removeLast();
    m_statesBehindStoresToPutBack.clear();
    if (m_waysIntoClosedCopies.isEmpty())
        m_storesToPutBack.shrink(0);
}

void Walker::noteStoreIntoElementToPutBack(Reference list, Reference position, Reference held)
{
    StoreToPutBack store;
    store.object = list;
    store.held = held;
    store.position = position;
    noteStoreBeingBuiltToPutBack(store);
}

void Walker::noteStoreIntoFieldOfHeirToPutBack(Reference heir, LayoutID layout, uint32_t field, Reference held)
{
    StoreToPutBack store;
    store.object = heir;
    store.held = held;
    store.layoutOfHeir = layout;
    store.fieldOfHeir = field;
    noteStoreBeingBuiltToPutBack(store);
}

void Walker::letRestOfStretchRunAsWrittenBehindFirstStore(ValueID store)
{
    StretchWithArmsAsWritten& stretch = m_stretchWithArmsAsWritten;
    if (!stretch.asWritten || stretch.restAsWritten || stretch.turnBeginsAgainInstead || stretch.numberOfFrames != m_frames.size() || isUnreachable() || store.index() < stretch.first || store.index() - stretch.first + 1 >= stretch.size)
        return;
    stretch.restAsWritten = addBlock(BlockFrequency::Unlikely);
    stretch.placeOfRest = store.index() - stretch.first + 1;
    for (uint32_t place = 0; place < stretch.placeOfRest; ++place)
        stretch.boundInFrontOfRest.append(referenceBoundTo(ValueID(stretch.first + place)));
    stretch.asWritten = stretch.restAsWritten;
    stretch.number = beginStretchInWhichNothingWasDone();
    count(StatisticID::RestsOfStretchesThatMayRunAsWritten);
}

void Walker::beginTurnAgainInClosedCopy()
{
    if (m_state.markOfTurn() & WalkState::storesOfTurnCanBePutBack) {
        for (size_t place = 0; place < m_waysIntoClosedCopies.last().variablesOfFilesHeldAtHead.size(); ++place) {
            auto [variable, held] = m_waysIntoClosedCopies.last().variablesOfFilesHeldAtHead[place];
            Draft store(Opcode::PutModuleVariable, m_origin);
            store.setVariable(variable);
            store.setValueOperand(held);
            chooseConservativeFields(store);
            build(WTF::move(store));
            if (m_hasFailed || isUnreachable())
                return;
            ++m_numberOfStoresPutBack;
        }
    }
    if (uint32_t newest = m_state.newestStoreToPutBack()) {
        SetForScope isPuttingStoresBack(m_isPuttingStoresBack, true);
        auto stateBehind = [&](uint32_t place) -> const WalkState* {
            auto kept = std::ranges::lower_bound(m_statesBehindStoresToPutBack, place, { }, [](const auto& state) { return state.first; });
            return kept != m_statesBehindStoresToPutBack.end() && kept->first == place ? &kept->second : nullptr;
        };
        auto blockOf = [&](uint32_t place) {
            const auto& known = m_waysIntoClosedCopies.last().blocksThatPutStoresBack;
            auto found = std::ranges::lower_bound(known, place, { }, [](const auto& block) { return block.first; });
            return found != known.end() && found->first == place ? found->second : BlockID();
        };
        bool eachStoreHasOneBlock = true;
        for (uint32_t place = newest; place && eachStoreHasOneBlock && !blockOf(place); place = m_storesToPutBack[place].previous)
            eachStoreHasOneBlock = !!stateBehind(place);
        for (uint32_t place = newest; place; place = m_storesToPutBack[place].previous) {
            if (eachStoreHasOneBlock) {
                if (BlockID known = blockOf(place)) {
                    jumpTo(known, { });
                    m_block = { };
                    m_state.clear();
                    ++m_numberOfTurnsThatBeginAgain;
                    return;
                }
                BlockID whereItIsPutBack = addBlock(BlockFrequency::Rare);
                jumpTo(whereItIsPutBack, { });
                if (m_hasFailed || isUnreachable())
                    return;
                auto& known = m_waysIntoClosedCopies.last().blocksThatPutStoresBack;
                known.insert(std::ranges::lower_bound(known, place, { }, [](const auto& block) { return block.first; }) - known.begin(), std::pair { place, whereItIsPutBack });
                m_state = *stateBehind(place);
            }
            StoreToPutBack store = m_storesToPutBack[place];
            RELEASE_ASSERT(store.numberOfTurn == m_waysIntoClosedCopies.last().numberOfTurn);
            if (store.position) {
                Draft element(Opcode::PutOwnDoubleIndex, m_origin);
                chooseConservativeFields(element);
                element.setArrayOperand(store.object);
                element.setPositionOperand(store.position);
                element.setValueOperand(store.held);
                element.setPutsBackWhatTurnStored(true);
                build(WTF::move(element));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_numberOfStoresPutBack;
                continue;
            }
            if (store.layoutOfHeir) {
                Draft property(Opcode::PutPropertyOfHeir, m_origin);
                chooseConservativeFields(property);
                property.setObjectOperand(store.object);
                property.setValueOperand(store.held);
                property.setLayout(store.layoutOfHeir);
                property.setField(store.fieldOfHeir);
                property.setPutsBackWhatTurnStored(true);
                build(WTF::move(property));
                if (m_hasFailed || isUnreachable())
                    return;
                ++m_numberOfStoresPutBack;
                continue;
            }
            Draft draft(Opcode::PutSlot, m_origin);
            chooseConservativeFields(draft);
            draft.setObjectOperand(store.object);
            draft.setValueOperand(store.held);
            draft.setKey(store.key);
            draft.setReceivers(store.receivers);
            draft.setLevel(KnowledgeLevel::Proven);
            draft.setMode(store.mode);
            draft.setIsDefinition(false);
            draft.setPutsBackWhatTurnStored(true);
            build(WTF::move(draft));
            if (m_hasFailed || isUnreachable())
                return;
            ++m_numberOfStoresPutBack;
        }
    }
    if (Reference head = m_waysIntoClosedCopies.last().headOfTurnInEffectsNet; head && head.isValue()) [[unlikely]] {
        Draft edge(Opcode::TurnBeginsAgain, m_origin);
        edge.setBeginningOperand(head);
        build(WTF::move(edge));
        if (m_hasFailed || isUnreachable())
            return;
    }
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    leaveForClosedCopy(inputBlock(way.frameIndex, way.inputHead).head, way.heldAtHead.span());
    m_block = { };
    m_state.clear();
    ++m_numberOfTurnsThatBeginAgain;
}

BitVector Walker::findLoopsThatReckonWithElements(const BodyIndex& index)
{
    BitVector loops;
    if (!index.holdsAnyOf(OpcodeSet { Opcode::ElemGet }))
        return loops;
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        LoopID loop = index.loopOf(BlockID(block));
        if (!loop || loops.get(loop.index()))
            continue;
        for (BodyValue operation : index.block(BlockID(block)).values()) {
            if (operation.opcode() != Opcode::ElemGet)
                continue;
            ValueID user = index.onlyUsingValue(operation.id());
            while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
                user = index.onlyUsingValue(user);
            if (!user || index.opcodeOf(user) == Opcode::Add || !allNumericOperators.contains(index.opcodeOf(user)))
                continue;
            loops.set(loop.index());
            break;
        }
    }
    return loops;
}

bool Walker::isTooLargeToBeBuiltTwice(const BodyIndex& index, LoopID loop, const BitVector& loopsThatReckonWithElements)
{
    uint32_t numberOfOperations = 0;
    for (BlockID member : index.membersOf(loop)) {
        numberOfOperations += index.block(member).numberOfValues() + 1;
        if (numberOfOperations > largestLoopBuiltTwice)
            return !loopsThatReckonWithElements.get(loop.index());
    }
    return false;
}

bool Walker::isTooLargeToBeBuiltTwice(const BodyIndex& index, LoopID loop)
{
    return isTooLargeToBeBuiltTwice(index, loop, m_loopsThatReckonWithElements.ensure(&index, [&] {
        return findLoopsThatReckonWithElements(index);
    }).iterator->value);
}

static bool turnsStoreAtPositionOfListInLoopsBuiltTwice(const BodyIndex& index, LoopID loop)
{
    if (!index.opcodes().contains(Opcode::TryPutByValDirect))
        return false;
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [](BodyValue operation) {
            return operation.opcode() == Opcode::TryPutByValDirect;
        });
    });
}

bool Walker::turnsOfTypedCopyStoreAtPositionOfList() const
{
    if (m_waysIntoClosedCopies.isEmpty())
        return false;
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    const BodyIndex& index = *m_frames[way.frameIndex].index;
    return turnsStoreAtPositionOfListInLoopsBuiltTwice(index, index.loopOf(way.inputHead));
}

bool Walker::mayBuildLoopSeveralTimes(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Guesses) || !m_opensGuesses || !index.isLoopHead(inputHead) || index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead))
        return false;
    if (index.holdsParametersReadByReleaseHintsAlone())
        return false;
    if (frameIndex && !m_frames[frameIndex].loopDepthAtCall && index.loopDepthOf(inputHead) == 1 && std::ranges::any_of(facts().linked().parametersOf(m_frames[frameIndex].function), &LinkedParameter::isOnlyCalled) && !turnsStoreAtPositionOfListInLoopsBuiltTwice(index, index.loopOf(inputHead)))
        return false;
    if (drawChoice(DrawnFor::LoopBuiltTwice, currentFunction().index(), inputHead.index(), false) == ChoiceOfCost::No)
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !iterationsSteppedAtMayBeBuiltTwice(inputHead))
        return false;

    if (isTooLargeToBeBuiltTwice(index, loop))
        return false;
    bool yieldsWalkTurnsOfConsumer = loopCarriesValuesOfConsumerRound(inputHead);
    if (yieldsWalkTurnsOfConsumer && !turnsOfConsumerFitLoopBuiltTwice(inputHead, largestLoopBuiltTwice))
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
            if (maySuspendAsWritten(operation) && !(yieldsWalkTurnsOfConsumer && operation.opcode() == Opcode::Yield))
                return false;
        }
    }
    return true;
}

std::span<const uint32_t> Walker::numbersOfHandlersThatRead(Frame& frame)
{
    std::span<const BlockID> handlerBlocks = handlerBlocksOf(frame);
    if (!handlerBlocks.empty() && frame.numberOfHandlersThatRead.isEmpty()) {
        if (!m_liveness.isComputedFor(*frame.index))
            m_liveness.compute(*frame.index);
        frame.numberOfHandlersThatRead.fill(0, frame.index->numberOfValues());
        m_liveness.countHeadsWhereLive(handlerBlocks, frame.numberOfHandlersThatRead.mutableSpan());
    }
    return frame.numberOfHandlersThatRead.span();
}

uint32_t Walker::numberOfHandlersThatRead(Frame& frame, ValueID value)
{
    return numbersOfHandlersThatRead(frame).empty() ? 0 : frame.numberOfHandlersThatRead[value.index()];
}

static bool mustBeNamedByDefinitionInLoopsBuiltTwice(const BodyIndex& index, ValueID value)
{
    std::optional<BodyUser> user = index.isParameter(value) ? std::nullopt : index.firstUser(value);
    if (!user)
        return false;
    auto namesByDefinition = [&](const auto& reader) {
        bool isFound = false;
        reader.forEachOperand([&](ValueID operand, uint32_t place) {
            isFound |= operand == value && mustBeDefinition(reader.operandKind(place));
        });
        return isFound;
    };
    if (user->kind == BodyUseKind::Operand)
        return namesByDefinition(index.value(user->value));
    if (user->kind == BodyUseKind::TerminatorOperand)
        return namesByDefinition(index.terminator(user->block));
    return user->kind == BodyUseKind::BlockSubject;
}

static BlockID cleanupOutsideLoopThatClosesIteration(Walker& walker, const BodyIndex& index, LoopID loop, ValueID open)
{
    bool isOpenedByPattern = !index.isParameter(open) && findListWhosePairsPatternTakesInParts(walker, index, index.value(open));
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        if (block.hasHandler() && !index.isInLoop(block.handler().block(), loop) && (isOpenedByPattern ? openOfCleanupThatThrowsOnOutsideLoop(index, block, loop) : openOfCleanupThatOnlyThrowsOn(index, block)) == open)
            return block.handler().block();
    }
    return { };
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
    m_valuesReadBehindLoopFoundLast.readByHandlersOutside.shrink(0);
    m_valuesReadBehindLoopFoundLast.cleanupsOutside.shrink(0);
    LoopID loop = index.loopOf(inputHead);
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    ValueID endOfValues = index.block(index.loop(loop).last).endOfValues();
    Vector<ValueID, 8> readBehindLoop;
    BitVector isListed;
    Vector<ValueID, 2> readByHandlersOutside;
    Vector<BlockID, 2> cleanupsOutside;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (index.isInLoop(target.block(), loop))
                continue;
            m_liveness.forEachLiveAtHead(target.block(), ValueID(firstValue), endOfValues, [&](ValueID live) {
                if (index.isInLoop(index.blockOf(live), loop) && isListed.add(live.index() - firstValue))
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
            if (!numberOutsideLoop)
                continue;
            if (!mustBeNamedByDefinitionInLoopsBuiltTwice(index, ValueID(value))) {
                readByHandlersOutside.append(ValueID(value));
                continue;
            }
            BlockID cleanup = numberOutsideLoop == 1 ? cleanupOutsideLoopThatClosesIteration(*this, index, loop, ValueID(value)) : BlockID();
            if (!cleanup)
                return std::nullopt;
            cleanupsOutside.append(cleanup);
        }
    }
    if (!readByHandlersOutside.isEmpty() || !cleanupsOutside.isEmpty()) {
        if (turnsOfLoopCallForGuesses(index, loop))
            return std::nullopt;
        for (ValueID value : readBehindLoop) {
            if (mustBeNamedByDefinitionInLoopsBuiltTwice(index, value))
                return std::nullopt;
        }
    }
    for (ValueID value : readByHandlersOutside) {
        ASSERT(value.index() >= firstValue);
        if (isListed.add(value.index() - firstValue))
            readBehindLoop.append(value);
    }
    std::ranges::sort(cleanupsOutside);
    m_valuesReadBehindLoopFoundLast.readByHandlersOutside = WTF::move(readByHandlersOutside);
    m_valuesReadBehindLoopFoundLast.cleanupsOutside = WTF::move(cleanupsOutside);
    m_valuesReadBehindLoopFoundLast.found = readBehindLoop;
    return readBehindLoop;
}

bool Walker::handlersOutsideLoopReadItsValues(BlockID inputHead)
{
    return findValuesOfLoopReadBehindIt(inputHead) && (!m_valuesReadBehindLoopFoundLast.readByHandlersOutside.isEmpty() || !m_valuesReadBehindLoopFoundLast.cleanupsOutside.isEmpty());
}

void Walker::walkCleanupsBesideCopyOfLoop(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const Frame& frame = m_frames[frameIndex];
    if (m_hasFailed || !frame.valuesBuiltTwice || frame.valuesBuiltTwice->cleanupsWalkedBesideCopiesOfLoops.isEmpty())
        return;
    Vector<BlockID, 2> cleanups;
    for (auto [loop, cleanup] : frame.valuesBuiltTwice->cleanupsWalkedBesideCopiesOfLoops) {
        if (loop == frame.index->loopOf(inputHead))
            cleanups.append(cleanup);
    }
    SetForScope walksCleanup(m_walksCleanupBesideCopyOfLoop, true);
    for (BlockID cleanup : cleanups) {
        if (m_hasFailed || m_frames[frameIndex].blocks[cleanup.index()].firstOpenEdge == noOpenEdge)
            continue;
        walkBlock(cleanup);
        inputBlock(frameIndex, cleanup) = InputBlock { };
    }
}

void Walker::noteValuesThatHandlersOutsideLoopRead(BlockID inputHead)
{
    if (!handlersOutsideLoopReadItsValues(inputHead))
        return;
    count(StatisticID::LoopsBuiltTwiceWhoseValuesHandlersOutsideRead);
    Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    LoopID loop = index.loopOf(inputHead);
    Frame::ValuesBuiltTwice& listed = frame.ensureValuesBuiltTwice();
    listed.cleanupsWalkedBesideCopiesOfLoops.removeAllMatching([&](const std::pair<LoopID, BlockID>& walked) {
        return !frame.loopsBeingBuiltTwice.contains(walked.first);
    });
    listed.valuesBoxedOnEdgesIntoHandlers.removeAllMatching([&](const std::pair<BlockID, ValueID>& boxed) {
        return frame.blocks[boxed.first.index()].isEntered;
    });
    for (BlockID cleanup : m_valuesReadBehindLoopFoundLast.cleanupsOutside)
        listed.cleanupsWalkedBesideCopiesOfLoops.append({ loop, cleanup });
    if (m_valuesReadBehindLoopFoundLast.readByHandlersOutside.isEmpty())
        return;
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    for (BlockID member : index.membersOf(loop)) {
        if (!index.block(member).hasHandler() || index.isInLoop(index.block(member).handler().block(), loop))
            continue;
        BlockID handler = index.block(member).handler().block();
        for (ValueID value : m_valuesReadBehindLoopFoundLast.readByHandlersOutside) {
            if (m_liveness.isLiveAtHead(value, handler) && !listed.valuesBoxedOnEdgesIntoHandlers.contains(std::pair { handler, value }))
                listed.valuesBoxedOnEdgesIntoHandlers.append({ handler, value });
        }
    }
}

bool Walker::boxValuesThatHandlerOutsideLoopReads(size_t frameIndex, BlockID handler, Vector<std::pair<ValueID, Reference>, 2>& names)
{
    const Frame& frame = m_frames[frameIndex];
    for (auto [reader, value] : frame.valuesBuiltTwice->valuesBoxedOnEdgesIntoHandlers) {
        if (reader != handler || !frame.isBuiltTwice(value))
            continue;
        Reference name = nameBehindStandIn(frameIndex, value, frame.lastNameOf(value));
        TypeID type = name ? ownTypeOf(name) : TypeID();
        if (!type)
            continue;
        Representation form = boxedFormOf(type);
        if (isSameWord(representationOf(name), form))
            continue;
        name = convert(name, form, m_origin);
        if (!name)
            return false;
        names.append({ value, name });
    }
    return true;
}

void Walker::endFrontOfLoopLeftOpen()
{
    FrontOfLoopLeftOpen front = std::exchange(m_frontOfLoopLeftOpen, { });
    if (!front.block || m_code.at(front.block).hasEnded())
        return;
    m_code.end(front.block, TerminatorDraft(TerminatorKind::Jump, front.origin), 1);
    m_code.closeEdge(m_code.at(front.block).terminator.next(), front.head, *front.entering);
}

Vector<uint32_t, 8> Walker::nameValuesOnEdgesThatLeavePlainWalk(BlockID inputHead, std::span<const ValueID> readBehindLoop)
{
    Vector<uint32_t, 8> edges;
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    Vector<ValueID, 8> listedBehindWalk;
    for (ValueID value : readBehindLoop) {
        if (m_frames[frameIndex].listAsBuiltTwice(value))
            listedBehindWalk.append(value);
    }
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
                    if (Reference bound = frame.references[value.index()]; bound && (!open.names.nameIn(slot) || listedBehindWalk.contains(value)))
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
            frame.references.placeOf<NamesOfValues::Writer::ForgetsValuesOfLoop>(value) = Reference();
    }
}

OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

static bool sameNamesLeadFromSameHolderInTurn(const BodyIndex& index, ValueID first, ValueID second, uint32_t firstValueOfLoop)
{
    first = index.valueBehindCasts(first);
    second = index.valueBehindCasts(second);
    if (first == second)
        return true;
    if (!first || !second || first.index() < firstValueOfLoop || second.index() < firstValueOfLoop || index.isParameter(first) || index.isParameter(second) || index.opcodeOf(first) != index.opcodeOf(second))
        return false;
    BodyValue one = index.value(first);
    BodyValue other = index.value(second);
    switch (one.opcode()) {
    case Opcode::CheckInitialized:
        return sameNamesLeadFromSameHolderInTurn(index, one.operand(0), other.operand(0), firstValueOfLoop);
    case Opcode::GetModuleVariable:
        return one.variable() == other.variable();
    case Opcode::GetEnvironmentField:
        return one.environmentOperand() == other.environmentOperand() && one.layout() == other.layout() && one.field() == other.field();
    case Opcode::PropGet:
        return !one.isFreeName() && !other.isFreeName() && one.key() == other.key() && sameNamesLeadFromSameHolderInTurn(index, one.objectOperand(), other.objectOperand(), firstValueOfLoop);
    default:
        return false;
    }
}

bool Walker::buildLoopForEachMember(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!m_buysTwoBuilds && !m_asksOnlyWhetherLoopMayBeBuiltTwice)
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (!m_otherCopiesInsideTypedCopies.isEmpty() && m_otherCopiesInsideTypedCopies.last() != std::pair { frameIndex, inputHead } && index.loop(loop).numberOfLoopsWithin > 1)
        return false;
    bool sharedCopyOfThisLoopWaits = m_closedCopiesShared.containsIf([&](const ClosedCopyShared& shared) { return shared.frameIndex == frameIndex && shared.inputHead == inputHead; });
    bool standsInCopyOfLoopAround = !m_closedCopiesShared.isEmpty();
    if (m_numberOfCopiesBuiltForSize || sharedCopyOfThisLoopWaits || (standsInCopyOfLoopAround && index.loop(loop).numberOfLoopsWithin > 1) || (isUnreachable() && !m_asksOnlyWhetherLoopMayBeBuiltTwice) || hasTextsInStringBuilders())
        return false;
    if (m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop))
        return false;
    IterationsSteppedAtHead steppedAtHead;
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead))) {
        steppedAtHead = takeIterationsSteppedAt(inputHead);
        if (steppedAtHead.asTheyEntered.size() != 1 || standsInCopyOfLoopAround)
            return false;
        const IterationByIndex& entered = steppedAtHead.asTheyEntered[0].second;
        if (!entered.textOrListMayStandInstead || !entered.mayGoByProtocol || !entered.isDeadBehindLoop || entered.state || entered.table || entered.contents != IterationByIndex::Contents::List || !entered.list || !entered.list.isValue())
            return false;
    }
    bool tellsTextFromListOfIteration = !steppedAtHead.asTheyEntered.isEmpty();
    std::optional<Vector<ValueID, 8>> readBehindLoop = findValuesOfLoopReadBehindIt(inputHead);
    if (!readBehindLoop)
        return false;
    size_t operatorsMadeInFront = m_operatorsMadeInFrontOfLoops.size();
    ValueID row = isUnreachable() || tellsTextFromListOfIteration ? ValueID() : findRowThatHeadReadsOffListInFront(*this, inputHead, preview);
    if (row && m_asksOnlyWhetherLoopMayBeBuiltTwice)
        return true;
    BlockID rowIsNotFound;
    if (row)
        rowIsNotFound = addBlock();
    RowFoundUnderKey operandsOfRow = row ? readVariablesOfRowInFrontOfLoop(*this, index.value(row), rowIsNotFound, m_origin) : m_rowOfCopyFoundUnderKey;
    SetForScope operandsOfRowInLooks(m_rowOfCopyFoundUnderKey, operandsOfRow);
    if (row) {
        Reference made = isUnreachable() || m_hasFailed ? Reference() : readRowInFrontOfLoop(*this, index.value(row), rowIsNotFound, m_origin);
        if (made && made.isValue() && !isUnreachable())
            noteOperatorMadeInFrontOfLoop(row, made);
    }
    auto goOnWithBothWays = [&] {
        if (!rowIsNotFound || m_hasFailed)
            return;
        BlockID goesOn = addBlock();
        if (!isUnreachable())
            jumpTo(goesOn, { });
        continueIn(rowIsNotFound);
        if (!isUnreachable())
            jumpTo(goesOn, { });
        continueIn(goesOn);
        rowIsNotFound = { };
    };
    Vector<GuessInFrontOfLoop, 4> members;
    if (tellsTextFromListOfIteration) {
        Reference held = steppedAtHead.asTheyEntered[0].second.list;
        members.append(GuessInFrontOfLoop { held, TypeID(TypeTable::string), { }, { } });
        members.append(GuessInFrontOfLoop { held, m_types.builtinClass(BuiltinClasses::Array), { }, { } });
    } else
        members = findMembersToldApartInFrontOfLoop(*this, inputHead, preview);
    m_operatorsMadeInFrontOfLoops.shrink(operatorsMadeInFront);
    if (!members.isEmpty() && members[0].slotReadInLoop && members[0].slotReadInLoop->holderIsProven && !members[0].slotReadInLoop->holderIsAskedWhetherItIsFilled && members[0].slotReadInLoop->slotsThatFindHolder.isEmpty()) {
        BodyValue written = index.value(members[0].slotReadInLoop->read);
        Draft read(Opcode::GetSlot, originOf(written));
        chooseConservativeFields(read);
        read.setObjectOperand(members[0].slotReadInLoop->nameOfHolder);
        read.setKey(keyOfAccessToSlot(program(), written));
        read.setReceivers(members[0].slotReadInLoop->layoutsOfHolder);
        read.setLevel(KnowledgeLevel::Proven);
        TypeID given = resultTypeOf(read);
        if (given && m_types.at(given).hasObjects() && !m_types.at(given).hasUnboundedUnknown() && !m_types.overlap(given, m_types.builtinClass(BuiltinClasses::Array)))
            members.shrink(0);
    }
    bool isToldApart = !members.isEmpty() && (members.size() >= 2 || members[0].nothingWasExpected || members[0].restIsWalkedAsWithoutGuess);
    if (!isToldApart || !members[0].readOfRowInLoop)
        goOnWithBothWays();
    if (!isToldApart)
        return false;
    if (standsInCopyOfLoopAround && !members[0].readOfRowInLoop) {
        const ClosedCopyShared& around = m_closedCopiesShared.last();
        bool isFoundInTurnAround = around.frameIndex == frameIndex && std::ranges::any_of(members[0].names, [&](ValueID name) {
            return index.isInLoop(index.blockOf(name), index.loopOf(around.inputHead));
        });
        if (!isFoundInTurnAround)
            return false;
    }
    if (m_asksOnlyWhetherLoopMayBeBuiltTwice)
        return true;
    SetForScope isBought(m_buysTwoBuilds, false);
    SetForScope ordinalOfCopy(m_ordinalOfCopyOfLoop, m_ordinalOfCopyOfLoop * 16 + 3);

    struct StartOfCopy {
        BlockID block;
        WalkState state;
        Reference proof;
        bool isListBehindListOfNumbers { false };
        bool asksOtherHoldersForLists { false };
        TypeID tested { };
        bool expectsOtherHoldersOfItsClass { false };
        bool expectsTablesToBeViews { false };
        bool expectsThoseWithWordOfListToo { false };
        Vector<std::pair<size_t, Reference>, 1> elementsThatEnterAsNumbers { };
        UncheckedKeyHashMap<uint32_t, Reference, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lengthsReadInFront { };
        bool asksBoundForText { false };
    };
    Vector<std::pair<ValueID, ValueID>, 2> namesOfLengthsReadInFront;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> namesAskedAbout;
    if (!members[0].slotReadInLoop && !members[0].readOfVariableInLoop && !members[0].readOfRowInLoop) {
        uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
        auto isLengthOfHolder = [&](ValueID read) {
            if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet)
                return false;
            BodyValue written = index.value(read);
            return !written.isFreeName() && program().nameClassOf(written.key()) == NameClass::Length && referenceOf(written.objectOperand()) == members[0].value;
        };
        auto nothingSeenIsWrittenUpToLoop = [&](ValueID read) {
            BlockID block;
            for (const BodyIncomingEdge& edge : incomingEdgesOf(index, inputHead)) {
                if (index.isInLoop(edge.from, loop))
                    continue;
                if (block)
                    return false;
                block = edge.from;
            }
            while (block) {
                bool holdsRead = index.blockOf(read) == block;
                for (BodyValue operation : index.block(block).values()) {
                    if (holdsRead && operation.id().index() <= read.index())
                        continue;
                    if (!declaredEffectsOfOperation(program(), operation.opcode(), operation).mayBeRunAgainUnseen())
                        return false;
                    bool someOperandMayBeObject = false;
                    if (operandsMayRunCode(operation.opcode())) {
                        operation.forEachOperand([&](ValueID operand, uint32_t) {
                            if (!operand || isLengthOfHolder(index.valueBehindCasts(operand)))
                                return;
                            Reference built = referenceOf(operand);
                            someOperandMayBeObject |= !built || !built.isRecord() || m_types.saysNothing(typeOf(built)) || m_types.at(typeOf(built)).hasObjects();
                        });
                    }
                    if (someOperandMayBeObject)
                        return false;
                }
                if (holdsRead)
                    return true;
                std::span<const BodyIncomingEdge> arriving = incomingEdgesOf(index, block);
                if (arriving.size() != 1 || index.isLoopHead(block) || index.isEnteredByHandlerEdge(block))
                    return false;
                block = arriving[0].from;
            }
            return false;
        };
        auto noteLengthReadInFront = [&](ValueID name, uint32_t) {
            if (!name || name.index() >= firstValueOfLoop || !namesAskedAbout.add(name.index()).isNewEntry)
                return;
            ValueID read = index.valueBehindCasts(name);
            if (!isLengthOfHolder(read) || !nothingSeenIsWrittenUpToLoop(read))
                return;
            namesOfLengthsReadInFront.append({ name, read });
        };
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values())
                operation.forEachOperand(noteLengthReadInFront);
            index.block(member).terminator().forEachOperand(noteLengthReadInFront);
        }
    }
    Origin origin = m_origin;
    Vector<StartOfCopy, 4> starts;
    bool listWasAskedForStorageOfNumbers = false;
    BlockID whereHolderOfSlotFails;
    Reference proofOfHolderOfSlot;
    Vector<Reference, 1> proofsOfHoldersAroundSlot;
    if (std::optional<SlotReadInLoopOffHolderInFront> slot = members[0].slotReadInLoop) {
        whereHolderOfSlotFails = addBlock(BlockFrequency::Rare);
        bool testOfHolderIsMissing = false;
        auto readSlotInFront = [&](Reference holder, ValueID readInLoop, TypeID tested, LayoutSetID layouts, bool holderIsProven) -> std::pair<Reference, Reference> {
            Reference proof = holder;
            if (!holderIsProven || !m_types.at(typeOf(holder)).isObject()) {
                GuessInFrontOfLoop ofHolder { holder, tested, layouts, { }, false, { }, std::nullopt };
                Reference test = buildTestInFrontOfLoop(*this, ofHolder, origin);
                if (!test || !test.isValue() || isUnreachable()) {
                    testOfHolderIsMissing = true;
                    return { };
                }
                BlockID passed = addBlock();
                branch(test, passed, whereHolderOfSlotFails);
                continueIn(passed);
                proof = buildProofInFrontOfLoop(*this, ofHolder, test, origin);
            }
            if (!proof || isUnreachable())
                return { };
            BodyValue written = index.value(readInLoop);
            Draft isFilled(Opcode::IsSlotFilled, originOf(written));
            chooseConservativeFields(isFilled);
            isFilled.setObjectOperand(proof);
            isFilled.setKey(keyOfAccessToSlot(program(), written));
            isFilled.setReceivers(layouts);
            isFilled.setLevel(KnowledgeLevel::Proven);
            Reference holdsIt = build(WTF::move(isFilled));
            if (holdsIt && holdsIt.isValue() && !isUnreachable()) {
                BlockID filled = addBlock();
                branch(holdsIt, filled, whereHolderOfSlotFails);
                continueIn(filled);
            }
            if (!holdsIt || isUnreachable())
                return { proof, Reference() };
            Draft read(Opcode::GetSlot, originOf(written));
            chooseConservativeFields(read);
            read.setObjectOperand(proof);
            read.setKey(keyOfAccessToSlot(program(), written));
            read.setReceivers(layouts);
            read.setLevel(KnowledgeLevel::Proven);
            return { proof, build(WTF::move(read)) };
        };
        Reference given = slot->nameOfHolder;
        for (const SlotThatFindsHolderOfSlotReadInLoop& around : slot->slotsThatFindHolder) {
            if (!given || !given.isValue())
                break;
            auto [proof, found] = readSlotInFront(given, around.read, around.testedOfHolder, around.layoutsOfHolder, around.holderIsProven && !around.holderIsAskedWhetherItIsFilled);
            proofsOfHoldersAroundSlot.append(proof);
            given = found;
        }
        if (given && given.isValue() && !isUnreachable()) {
            auto [proof, found] = readSlotInFront(given, slot->read, slot->testedOfHolder, slot->layoutsOfHolder, slot->holderIsProven && !slot->holderIsAskedWhetherItIsFilled);
            proofOfHolderOfSlot = proof;
            given = found;
        } else
            given = { };
        if (testOfHolderIsMissing && proofsOfHoldersAroundSlot.isEmpty() && !proofOfHolderOfSlot)
            return false;
        if (m_hasFailed)
            return true;
        if (given && given.isValue() && !isUnreachable() && m_types.at(typeOf(given)).hasObjects() && !m_types.at(typeOf(given)).hasUnboundedUnknown()) {
            members.removeAllMatching([&](const GuessInFrontOfLoop& member) {
                return !m_types.overlap(member.tested, typeOf(given));
            });
        }
        if (!given || !given.isValue() || isUnreachable() || members.isEmpty()) {
            BlockID joined = addBlock();
            if (!isUnreachable())
                jumpTo(joined, { });
            continueIn(whereHolderOfSlotFails);
            jumpTo(joined, { });
            continueIn(joined);
            return false;
        }
        for (GuessInFrontOfLoop& member : members)
            member.value = given;
    }
    for (const GuessInFrontOfLoop& member : members) {
        Reference test = isUnreachable() ? Reference() : buildTestInFrontOfLoop(*this, member, origin);
        if (!test)
            continue;
        BlockID passed = addBlock();
        BlockID next = addBlock();
        branch(test, passed, next);
        continueIn(passed);
        Reference proof = buildProofInFrontOfLoop(*this, member, test, origin);
        const ClassPart* part = proof && proof.isValue() && !isUnreachable() && m_types.at(typeOf(proof)).isObject() ? singleBuiltinClassOf(m_types, m_types.at(typeOf(proof))) : nullptr;
        if (part && pristineTestOf(program().builtins(), part->builtinClass) == PristineTest::InstanceStructure) {
            PristineAspectSet aspects { PristineAspects::Names };
            if (program().admitsReplacedBuiltins)
                aspects.add(PristineAspects::CellAlone);
            Draft isToldByItsStructure(Opcode::IsPristine, origin);
            chooseConservativeFields(isToldByItsStructure);
            isToldByItsStructure.setValueOperand(proof);
            isToldByItsStructure.setBuiltinClass(part->builtinClass);
            isToldByItsStructure.setAspects(aspects);
            Reference hasStructure = build(WTF::move(isToldByItsStructure));
            if (hasStructure && hasStructure.isValue() && !isUnreachable()) {
                BlockID hasIt = addBlock();
                branch(hasStructure, hasIt, next);
                continueIn(hasIt);
            }
        }
        bool isList = part && part->builtinClass == BuiltinClasses::Array;
        bool widerListFollows = std::ranges::any_of(members, [&](const GuessInFrontOfLoop& other) {
            return &other > &member && other.tested != member.tested && m_types.isSubset(member.tested, other.tested);
        });
        if (isList && !isUnreachable() && isDeclaredToHoldNumbersAlone(program(), m_types, typeOf(proof)) && widerListFollows) {
            Draft holdsNumbers(Opcode::HasArrayStorage, origin);
            chooseConservativeFields(holdsNumbers);
            holdsNumbers.setArrayOperand(proof);
            holdsNumbers.setArrayStorage(ArrayStorage::Double);
            holdsNumbers.setAdmitsInt32Storage(true);
            holdsNumbers.setIsWritable(false);
            Reference hasKind = build(WTF::move(holdsNumbers));
            if (hasKind && hasKind.isValue() && !isUnreachable()) {
                BlockID hasIt = addBlock();
                branch(hasKind, hasIt, next);
                continueIn(hasIt);
                listWasAskedForStorageOfNumbers = true;
                isList = false;
            }
        }
        Vector<std::pair<size_t, Reference>, 1> elementsThatEnterAsNumbers;
        for (size_t place = 0; part && part->builtinClass == BuiltinClasses::Array && !isList && place < entering.size() && !isUnreachable(); ++place) {
            Reference element = entering[place];
            if (!element || !element.isValue() || !(m_code.at(element.id()).opcode == Opcode::ElemGet ? m_code.at(element.id()).objectOperand() == member.value.id() : m_code.at(element.id()).opcode == Opcode::GetArrayElement && m_code.at(element.id()).arrayOperand() == member.value.id()))
                continue;
            GuessInFrontOfLoop isNumber { element, TypeID(TypeTable::number), { }, { } };
            Reference holdsNumber = buildTestInFrontOfLoop(*this, isNumber, origin);
            if (!holdsNumber || !holdsNumber.isValue())
                continue;
            BlockID holdsIt = addBlock();
            branch(holdsNumber, holdsIt, next);
            continueIn(holdsIt);
            if (Reference number = buildProofInFrontOfLoop(*this, isNumber, holdsNumber, origin)) {
                elementsThatEnterAsNumbers.append({ place, number });
                count(StatisticID::ElementsThatEnterLoopsAskedForNumbers);
            }
        }
        UncheckedKeyHashMap<uint32_t, Reference, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lengthsReadInFront;
        for (auto [name, read] : namesOfLengthsReadInFront) {
            if (!proof || !proof.isValue() || isUnreachable() || m_hasFailed || !part || part->builtinClass != BuiltinClasses::Array)
                break;
            if (lengthsReadInFront.contains(read.index()))
                continue;
            Reference asRead = referenceOf(read);
            if (!asRead || !asRead.isValue() || m_types.at(typeOf(asRead)).isInteger())
                continue;
            BodyValue written = index.value(read);
            Reference holderBefore = referenceBoundTo(written.objectOperand());
            Reference readBefore = referenceBoundTo(read);
            bind(written.objectOperand(), proof);
            Reference length;
            {
                ClosedArm takesNamesBack(*this);
                walkOperation(written);
                length = referenceOf(read);
            }
            bind(written.objectOperand(), holderBefore);
            bind(read, readBefore);
            if (m_hasFailed)
                return true;
            if (!length || !length.isValue() || length == asRead || isUnreachable() || !m_types.at(typeOf(length)).isInteger())
                continue;
            Reference isInt32 = buildTestOfNumber(*this, asRead, TypeTable::int32, origin);
            if (!isInt32 || !isInt32.isValue())
                continue;
            BlockID isOne = addBlock();
            branch(isInt32, isOne, next);
            continueIn(isOne);
            Draft isStillLength(Opcode::StrictEq, origin);
            chooseConservativeFields(isStillLength);
            isStillLength.setLeftOperand(buildProofOfNumber(*this, asRead, isInt32, origin));
            isStillLength.setRightOperand(length);
            isStillLength.setForm(formAllowedForTypedOperator(*this, isStillLength));
            Reference isSame = build(WTF::move(isStillLength));
            if (!isSame || !isSame.isValue() || isUnreachable())
                continue;
            BlockID itIs = addBlock();
            branch(isSame, itIs, next);
            continueIn(itIs);
            lengthsReadInFront.add(read.index(), length);
            count(StatisticID::LengthsReadInFrontOfLoopsAskedWhetherTheyStillHold);
        }
        BlockID firstStart = m_block;
        bool isOfViewsAlone = m_types.at(member.tested).isObject() && m_types.at(member.tested).hasOnlyClassObjects() && !m_types.overlap(member.tested, m_types.builtinClass(BuiltinClasses::Array));
        bool isListOfNumbers = proof && part && part->builtinClass == BuiltinClasses::Array && isDeclaredToHoldNumbersAlone(program(), m_types, typeOf(proof));
        if (proof && !isUnreachable() && (isOfViewsAlone || isListOfNumbers) && !findTablesThatMayBeViewsForGuesses(*this, inputHead, member.value).isEmpty()) {
            starts.append(StartOfCopy { m_block, m_state, proof, false });
            starts.last().expectsTablesToBeViews = true;
            firstStart = addBlock();
        }
        if (proof && !isUnreachable() && part && part->builtinClass == BuiltinClasses::Array && isDeclaredToHoldNumbersAlone(program(), m_types, typeOf(proof)) && otherHolderOfLoopMayBeListForGuesses(*this, inputHead, member.value)) {
            starts.append(StartOfCopy { firstStart, m_state, proof, isList && listWasAskedForStorageOfNumbers, true });
            starts.append(StartOfCopy { addBlock(), m_state, proof, isList && listWasAskedForStorageOfNumbers });
        } else if (size_t numberOfOthers = proof && !isUnreachable() && (member.tested == TypeID(TypeTable::string) || (m_types.at(member.tested).isObject() && m_types.at(member.tested).hasOnlyClassObjects() && !m_types.overlap(member.tested, m_types.builtinClass(BuiltinClasses::Array)))) ? findOtherHoldersExpectedOfClassOfFirstForGuesses(*this, inputHead, member.value, member.tested, member.tested != TypeID(TypeTable::string)).size() : 0) {
            size_t numberOfReaders = member.tested == TypeID(TypeTable::string) ? numberOfOthers : findOtherHoldersExpectedOfClassOfFirstForGuesses(*this, inputHead, member.value, member.tested).size();
            if (numberOfOthers > numberOfReaders) {
                starts.append(StartOfCopy { firstStart, m_state, proof, false });
                starts.last().expectsOtherHoldersOfItsClass = true;
                starts.last().expectsThoseWithWordOfListToo = true;
                firstStart = addBlock();
            }
            if (numberOfReaders) {
                starts.append(StartOfCopy { firstStart, m_state, proof, false });
                starts.last().expectsOtherHoldersOfItsClass = true;
                firstStart = addBlock();
            }
            starts.append(StartOfCopy { firstStart, m_state, proof, false });
        } else if (proof && !isUnreachable() && m_types.at(member.tested).isObject() && m_types.at(member.tested).hasOnlyClassObjects() && !m_types.overlap(member.tested, m_types.builtinClass(BuiltinClasses::Array)) && otherHolderOfLoopMayBeListForGuesses(*this, inputHead, member.value)) {
            starts.append(StartOfCopy { firstStart, m_state, proof, false, true });
            starts.append(StartOfCopy { addBlock(), m_state, proof, false });
            count(StatisticID::ListsAskedForBesideViewsInFrontOfLoops);
        } else if (proof && !isUnreachable() && (isOfViewsAlone || isListOfNumbers) && headOnlyMeasuresConstantOfFileForGuesses(*this, inputHead, member.value)) {
            starts.append(StartOfCopy { firstStart, m_state, proof, isList && listWasAskedForStorageOfNumbers, true });
            starts.append(StartOfCopy { addBlock(), m_state, proof, isList && listWasAskedForStorageOfNumbers });
            starts.last().asksBoundForText = true;
        } else if (proof && !isUnreachable())
            starts.append(StartOfCopy { firstStart, m_state, proof, isList && listWasAskedForStorageOfNumbers });
        for (StartOfCopy& start : starts) {
            if (!start.tested) {
                start.tested = member.tested;
                start.elementsThatEnterAsNumbers = elementsThatEnterAsNumbers;
                start.lengthsReadInFront = lengthsReadInFront;
            }
        }
        continueIn(next);
    }
    goOnWithBothWays();
    if (m_hasFailed)
        return true;
    noteGuessOpened();
    ++m_numberOfLoopsBuiltTwice;
    m_numberOfLoopsThatHoldLoopsBuiltTwice += index.loop(loop).numberOfLoopsWithin > 1;
    auto someMemberIs = [&](BuiltinClassID builtinClass) {
        return members.containsIf([&](const GuessInFrontOfLoop& member) { return m_types.isSubset(member.tested, m_types.builtinClass(builtinClass)) || m_types.isSubset(m_types.builtinClass(builtinClass), member.tested); });
    };
    bool tellsListFromBytes = someMemberIs(BuiltinClasses::Array) && someMemberIs(BuiltinClasses::Uint8Array);
    if (tellsListFromBytes)
        count(StatisticID::LoopsBuiltForListsAndForBytes);
    if (standsInCopyOfLoopAround)
        count(StatisticID::InnermostLoopsToldApartInCopiesOfLoopsAround);
    if (starts.containsIf([](const StartOfCopy& start) { return start.isListBehindListOfNumbers; }))
        count(StatisticID::LoopsWithCopyForListsOfAnything);
    if (members.containsIf([&](const GuessInFrontOfLoop& member) { return !!classOfViewBesideInstancesBelowIt(*this, member.tested); }))
        count(StatisticID::LoopsWhereViewAndItsHeirShareCopy);
    if (someMemberIs(BuiltinClasses::Array) && members.containsIf([&](const GuessInFrontOfLoop& member) { return member.tested == TypeID(TypeTable::string); }))
        count(tellsTextFromListOfIteration ? StatisticID::LoopsOfIterationsBuiltForTextsAndForLists : StatisticID::LoopsBuiltForListsAndForTexts);
    bool asksForListAlone = members[0].nothingWasExpected && !isDeclaredToHoldNumbersAlone(program(), m_types, members.last().tested) && std::ranges::all_of(members, [&](const GuessInFrontOfLoop& member) {
        return m_types.isSubset(member.tested, m_types.builtinClass(BuiltinClasses::Array));
    });
    if (asksForListAlone)
        count(StatisticID::LoopsBuiltForListsThatTheirCounterReads);

    if (members[0].slotReadInLoop && m_types.at(members[0].tested).hasOnlyClosures())
        count(StatisticID::LoopsBuiltForClosuresReadOutOfSlots);

    BlockID closedStart = addBlock(BlockFrequency::Rare);
    BlockID startOfRest;
    bool isReadInSomeTurnsOnly = !members[0].names.isEmpty() && !members[0].slotReadInLoop && !members[0].readOfVariableInLoop && !members[0].readOfRowInLoop;
    if (isReadInSomeTurnsOnly) {
        BlocksThatEveryTurnPasses passedByEveryTurn(index, loop);
        for (BlockID member : index.membersOf(loop)) {
            if (!passedByEveryTurn.contains(member))
                continue;
            auto mentions = [&](ValueID operand, uint32_t) {
                isReadInSomeTurnsOnly &= !members[0].names.contains(operand);
            };
            for (BodyValue operation : index.block(member).values())
                operation.forEachOperand(mentions);
            index.block(member).terminator().forEachOperand(mentions);
        }
    }
    BlockID startForWhatIsNoBuiltin;
    WalkState atEndOfTests;
    bool hasRest = (((tellsListFromBytes || asksForListAlone) && members[0].nothingWasExpected) || isReadInSomeTurnsOnly || members[0].restIsWalkedAsWithoutGuess) && !isUnreachable();
    auto onlyOperatorsAndSuchReadsRanOtherCode = [&] {
        SetForScope asInThatCopy(m_walksCopyForWhatIsNoBuiltin, true);
        SetForScope ofThisHolder(m_holderThatIsNoBuiltin, HolderThatIsNoBuiltin { members[0].names.span(), members[0].value, frameIndex, members[0].readOfVariableInLoop });
        return !m_operationsThatRanOtherCodeOfRow.isEmpty() && std::ranges::all_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            if (!ran || !index.isInLoop(index.blockOf(ran), loop))
                return false;
            BodyValue operation = index.value(ran);
            return effectRuleOf(operation.opcode()) == EffectRule::Operator || siteMayAnswerReadWithoutOtherCode(*this, operation);
        });
    };
    if (tellsListFromBytes && steppedAtHead.asTheyEntered.isEmpty() && !isUnreachable() && onlyOperatorsAndSuchReadsRanOtherCode()) {
        startForWhatIsNoBuiltin = addBlock(BlockFrequency::Rare);
        atEndOfTests = m_state;
        jumpTo(startForWhatIsNoBuiltin, { });
        starts.append(StartOfCopy { startForWhatIsNoBuiltin, m_state, { } });
    }
    if (hasRest) {
        startOfRest = addBlock();
        if (!startForWhatIsNoBuiltin)
            jumpTo(startOfRest, { });
        starts.append(StartOfCopy { startOfRest, { }, { } });
    }
    if (startForWhatIsNoBuiltin || startOfRest)
        continueIn(closedStart);
    else
        jumpTo(closedStart, { });
    if (whereHolderOfSlotFails && !m_hasFailed) {
        continueIn(whereHolderOfSlotFails);
        jumpTo(closedStart, { });
    }
    if (m_hasFailed)
        return true;
    m_block = closedStart;
    if (startForWhatIsNoBuiltin && !whereHolderOfSlotFails)
        m_state = WTF::move(atEndOfTests);
    BlockID entryOfClosedCopy = addBlock(BlockFrequency::Rare);
    m_entriesOfClosedCopies.add(entryOfClosedCopy.index());
    Vector<Reference, 8> enteringClosedCopy;
    for (size_t place = 0; place < entering.size(); ++place) {
        TypeID type = joinTypes(preview.typesOfParameters[place], ownTypeOf(entering[place]));
        enteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
    }
    takeFieldsOfScopesIntoClosedCopy(entryOfClosedCopy, preview);
    settleParametersOf(entryOfClosedCopy);

    for (ValueID value : *readBehindLoop)
        m_frames[frameIndex].listAsBuiltTwice(value);
    noteValuesThatHandlersOutsideLoopRead(inputHead);
    m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
    uint32_t namesInFrontOfLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;

    forgetAllButFreshObjectsAndFieldsThatBlockTakes(entryOfClosedCopy);
    enterWithFieldsOfScopes(entryOfClosedCopy, entering);
    enterClosedCopy(entryOfClosedCopy, preview);
    {
        ClosedArm closedArm(*this);
        SetForScope isBuiltForSize(m_numberOfCopiesBuiltForSize, m_numberOfCopiesBuiltForSize + !tellsListFromBytes);
        SetForScope holderIsNoBuiltin(m_walksCopyWhereHolderIsNoBuiltin, m_walksCopyWhereHolderIsNoBuiltin || tellsListFromBytes);
        walkCopyOfLoop(inputHead, enteringClosedCopy.span());
    }
    forgetValuesOfLoop(inputHead);

    m_closedCopiesShared.append(ClosedCopyShared { frameIndex, inputHead, closedStart, entryOfClosedCopy, false });
    LoopLookedAt lookedAtBehindClosedCopy = m_frames[frameIndex].lookedAt;
    bool turnsOfSomeCopyComeRound = false;
    for (StartOfCopy& start : starts) {
        if (m_hasFailed)
            break;
        if (m_frames[frameIndex].lookedAt.loop != loop)
            m_frames[frameIndex].lookedAt = lookedAtBehindClosedCopy;
        bool isRest = startOfRest && start.block == startOfRest;
        bool isForWhatIsNoBuiltin = startForWhatIsNoBuiltin && start.block == startForWhatIsNoBuiltin;
        if (isRest) {
            m_block = { };
            m_state.clear();
            PendingBlock* pending = findPendingBlock(startOfRest);
            size_t numberOfArrivals = pending ? pending->numberOfArrivals : 0;
            size_t numberOfEdges = 0;
            m_code.forEachEdgeIntoSoFar(startOfRest, [&](const Edge&) {
                ++numberOfEdges;
            });
            if (!numberOfEdges)
                continue;
            continueIn(startOfRest);
            if (numberOfEdges > numberOfArrivals) {
                m_block = startOfRest;
                m_state.clear();
            }
        } else {
            m_block = start.block;
            m_state = WTF::move(start.state);
        }
        WalkState atEndOfTestsForCopy;
        BlockID firstBlockOfCopyForWhatIsNoBuiltin;
        uint32_t firstValueOfCopyForWhatIsNoBuiltin = m_code.numberOfValues();
        if (isForWhatIsNoBuiltin) {
            atEndOfTestsForCopy = m_state;
            firstBlockOfCopyForWhatIsNoBuiltin = addBlock(BlockFrequency::Rare);
            m_block = firstBlockOfCopyForWhatIsNoBuiltin;
        }
        BlockID whereFrontFails = start.asksOtherHoldersForLists || start.expectsOtherHoldersOfItsClass || start.expectsTablesToBeViews ? (&start + 1)->block : startOfRest && !isRest && !isForWhatIsNoBuiltin ? startOfRest : closedStart;
        m_closedCopiesShared.last().start = whereFrontFails;
        m_closedCopiesShared.last().otherHoldersAreAskedForLists = start.asksOtherHoldersForLists;
        m_closedCopiesShared.last().boundIsAskedForText = start.asksBoundForText;
        if (whereFrontFails == startOfRest)
            count(StatisticID::CopiesOfLoopsWhoseFrontLeadsToTheRest);
        putBackIterationsSteppedAt(steppedAtHead);
        for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
            IterationByIndex& iteration = m_iterationsByIndex[place];
            bool isText = start.tested == TypeID(TypeTable::string);
            iteration.contents = isText ? IterationByIndex::Contents::Text : IterationByIndex::Contents::List;
            iteration.builtinClass = isText ? BuiltinClasses::String : BuiltinClasses::Array;
            iteration.list = start.proof;
            iteration.mayGoByProtocol = false;
            iteration.textOrListMayStandInstead = false;
            iteration.iterator = { };
            iteration.next = { };
            iteration.wayToStep = { };
        }
        Vector<std::pair<ValueID, Reference>, 8> boundBefore;
        for (ValueID name : members[0].names) {
            if (!start.proof)
                break;
            boundBefore.append({ name, referenceBoundTo(name) });
            bind(name, start.proof);
        }
        for (auto [name, read] : namesOfLengthsReadInFront) {
            auto taken = start.lengthsReadInFront.find(read.index());
            if (taken == start.lengthsReadInFront.end())
                continue;
            boundBefore.append({ name, referenceBoundTo(name) });
            bind(name, taken->value);
        }
        if (start.proof && members[0].slotReadInLoop) {
            const SlotReadInLoopOffHolderInFront& slot = *members[0].slotReadInLoop;
            if (!slot.holderIsReadOffVariableInLoop) {
                boundBefore.append({ slot.holder, referenceBoundTo(slot.holder) });
                bind(slot.holder, proofsOfHoldersAroundSlot.isEmpty() ? proofOfHolderOfSlot : proofsOfHoldersAroundSlot[0]);
            } else if (ValueID variableOfHolder = readOfVariableThatFindsReceiverInLoop(index, index.value(slot.slotsThatFindHolder.isEmpty() ? slot.read : slot.slotsThatFindHolder[0].read), index.block(inputHead).firstParameter().index())) {
                BodyValue read = index.value(variableOfHolder);
                Reference filled = proofsOfHoldersAroundSlot.isEmpty() ? proofOfHolderOfSlot : proofsOfHoldersAroundSlot[0];
                if (read.opcode() == Opcode::GetModuleVariable) {
                    if (program().at(read.variable()).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(program(), read.variable()))
                        m_state.noteEndOfDeadZone(read.variable());
                    m_state.noteVariable(read.variable(), filled);
                    noteValueOfStableBinding(*this, read.variable(), filled);
                } else if (Reference environment = referenceOf(read.environmentOperand())) {
                    FieldReference field { read.layout(), read.field() };
                    m_state.noteEndOfDeadZone(environment, field);
                    noteEnvironmentField(environment, field, filled);
                    noteValueOfStableBinding(*this, environment, field, filled);
                }
            }
            for (size_t place = 0; place < proofsOfHoldersAroundSlot.size(); ++place)
                m_state.noteSlot(proofsOfHoldersAroundSlot[place], slot.slotsThatFindHolder[place].field, place + 1 < proofsOfHoldersAroundSlot.size() ? proofsOfHoldersAroundSlot[place + 1] : proofOfHolderOfSlot);
            m_state.noteSlot(proofOfHolderOfSlot, slot.field, start.proof);
        }
        bool rowIsFoundUnderKey = start.proof && members[0].readOfRowInLoop && operandsOfRow.holder && operandsOfRow.key && !readTakesIndexOfProvenList(m_types, typeOf(operandsOfRow.holder), typeOf(operandsOfRow.key));
        SetForScope rowOfCopy(m_rowOfCopyFoundUnderKey, start.proof && members[0].readOfRowInLoop ? RowFoundUnderKey { operandsOfRow.holder, operandsOfRow.key, rowIsFoundUnderKey ? start.proof : Reference(), operandsOfRow.variableOfHolder, operandsOfRow.variableOfKey } : RowFoundUnderKey { });
        if (start.proof && members[0].readOfRowInLoop)
            noteRowReadInFrontOfLoop(*this, index.value(members[0].readOfRowInLoop), start.proof);
        if (start.proof && members[0].readOfVariableInLoop) {
            BodyValue read = index.value(members[0].readOfVariableInLoop);
            count(StatisticID::ReceiversReadOffVariablesToldApartInFrontOfLoops);
            if (read.opcode() == Opcode::GetModuleVariable) {
                m_state.noteEndOfDeadZone(read.variable());
                m_state.noteVariable(read.variable(), start.proof);
                noteValueOfStableBinding(*this, read.variable(), start.proof);
            } else if (Reference environment = referenceOf(read.environmentOperand())) {
                FieldReference field { read.layout(), read.field() };
                m_state.noteEndOfDeadZone(environment, field);
                noteEnvironmentField(environment, field, start.proof);
                noteValueOfStableBinding(*this, environment, field, start.proof);
            }
        }
        Vector<std::pair<ValueID, RememberedExpectation>, 2> expectedBefore;
        if (start.expectsOtherHoldersOfItsClass) {
            for (ValueID other : findOtherHoldersExpectedOfClassOfFirstForGuesses(*this, inputHead, members[0].value, start.tested, start.expectsThoseWithWordOfListToo)) {
                RememberedExpectation& remembered = rememberedExpectationOf(other);
                expectedBefore.append({ other, remembered });
                remembered = { start.tested, true, false, true };
            }
        }
        if (start.expectsTablesToBeViews) {
            for (ValueID table : findTablesThatMayBeViewsForGuesses(*this, inputHead, members[0].value)) {
                RememberedExpectation& remembered = rememberedExpectationOf(table);
                expectedBefore.append({ table, remembered });
                remembered = { viewsOfNumbersOfEveryKindForGuesses(m_types), true, false, true };
            }
        }
        if (start.proof && start.tested && members[0].slotReadInLoop) {
            BodyValue written = index.value(members[0].slotReadInLoop->read);
            for (BlockID member : index.membersOf(loop)) {
                for (BodyValue operation : index.block(member).values()) {
                    bool readsSlot = operation.opcode() == Opcode::PrivateGet || (operation.opcode() == Opcode::PropGet && !operation.isFreeName());
                    if (!readsSlot || !sameNamesLeadFromSameHolderInTurn(index, operation.objectOperand(), written.objectOperand(), index.block(inputHead).firstParameter().index()))
                        continue;
                    if (keyOfAccessToSlot(program(), operation) != keyOfAccessToSlot(program(), written) && m_types.at(start.tested).hasOnlyClosures())
                        continue;
                    if (keyOfAccessToSlot(program(), operation) != keyOfAccessToSlot(program(), written) && (members[0].slotReadInLoop->holderIsProven ? !readFindsListThatTurnsReckonWithForGuesses(*this, operation.id()) : !wordOfFieldThatFindsValueIsDrawnFromUseForGuesses(*this, operation.id())))
                        continue;
                    RememberedExpectation& remembered = rememberedExpectationOf(operation.id());
                    expectedBefore.append({ operation.id(), remembered });
                    remembered = { start.tested, true, false, true };
                }
            }
        }
        if (start.proof && start.tested && members[0].readOfVariableInLoop) {
            BodyValue written = index.value(members[0].readOfVariableInLoop);
            for (BlockID member : index.membersOf(loop)) {
                for (BodyValue operation : index.block(member).values()) {
                    if (operation.opcode() != written.opcode())
                        continue;
                    if (written.opcode() == Opcode::GetModuleVariable ? operation.variable() != written.variable() : operation.environmentOperand() != written.environmentOperand() || operation.layout() != written.layout() || operation.field() != written.field())
                        continue;
                    RememberedExpectation& remembered = rememberedExpectationOf(operation.id());
                    expectedBefore.append({ operation.id(), remembered });
                    remembered = { start.tested, true, false, true };
                }
            }
        }
        SetForScope listAskedInVain(m_listAskedForStorageOfNumbersInVain, start.isListBehindListOfNumbers ? start.proof : Reference());
        m_closedCopiesShared.last().isAboutToBeTaken = true;
        m_waysIntoClosedCopies.append(WayIntoClosedCopy { frameIndex, inputHead, entryOfClosedCopy, { }, whereFrontFails, { }, false });
        m_waysIntoClosedCopies.last().turnsCall = someOperationOfRowThatRanOtherCodeIsCall(index), m_waysIntoClosedCopies.last().someCallStaysInTurn = m_someCallStaysInTurnOfRow;
        m_waysIntoClosedCopies.last().reckonsWithWhatItReads = loopReckonsWithWhatItReads(inputHead);
        m_waysIntoClosedCopies.last().lastBlockThatEveryTurnPasses = BlocksThatEveryTurnPasses(index, index.loopOf(inputHead)).lastOf(index.loopOf(inputHead));
        ++m_ordinalOfCopyOfLoop;
        Vector<Reference, 8> enteringCopy(entering);
        for (auto [place, number] : start.elementsThatEnterAsNumbers)
            enteringCopy[place] = number;
        m_everyTurnOfTypedBuildLeaves = false;
        SetForScope holderIsNoBuiltin(m_walksCopyWhereHolderIsNoBuiltin, m_walksCopyWhereHolderIsNoBuiltin || ((isRest || isForWhatIsNoBuiltin) && tellsListFromBytes));
        SetForScope isNoBuiltin(m_walksCopyForWhatIsNoBuiltin, isForWhatIsNoBuiltin);
        SetForScope whichIsNoBuiltin(m_holderThatIsNoBuiltin, isForWhatIsNoBuiltin ? HolderThatIsNoBuiltin { members[0].names.span(), members[0].value, frameIndex, members[0].readOfVariableInLoop } : HolderThatIsNoBuiltin { });
        walkCopyOfLoop(inputHead, enteringCopy.span());
        turnsOfSomeCopyComeRound |= !std::exchange(m_everyTurnOfTypedBuildLeaves, false);
        dropWayIntoClosedCopy();
        m_closedCopiesShared.last().isAboutToBeTaken = false;
        for (auto [name, reference] : boundBefore)
            bind(name, reference);
        for (auto& [other, before] : expectedBefore)
            rememberedExpectationOf(other) = before;
        forgetValuesOfLoop(inputHead);
        if (isForWhatIsNoBuiltin && !m_hasFailed) {
            bool readsPlainStorage = false;
            for (uint32_t value = firstValueOfCopyForWhatIsNoBuiltin; value < m_code.numberOfValues() && !readsPlainStorage; ++value)
                readsPlainStorage = m_code.at(ValueID(value)).opcode == Opcode::GetElementInStorageOrAbsent;
            m_block = start.block;
            m_state = WTF::move(atEndOfTestsForCopy);
            jumpTo(readsPlainStorage ? firstBlockOfCopyForWhatIsNoBuiltin : closedStart, { });
            count(readsPlainStorage ? StatisticID::LoopsWithCopyForWhatIsNeitherListNorView : StatisticID::CopiesForWhatIsNeitherListNorViewWalkedAndDropped);
            m_block = { };
            m_state.clear();
        }
    }
    putBackIterationsSteppedAt(steppedAtHead);
    m_everyTurnOfTypedBuildLeaves = !turnsOfSomeCopyComeRound;
    m_closedCopiesShared.removeLast();
    forgetWhatWasBelievedAt(entryOfClosedCopy);

    for (BlockID member : index.membersOf(loop))
        inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
    m_frames[frameIndex].loopsBeingBuiltTwice.removeLast();
    m_block = { };
    m_state.clear();
    return true;
}

static bool turnsReadListsFoundFromInFront(const Program& program, const BodyIndex& index, BlockID inputHead)
{
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    WhatReadsFromInFrontOfLoopFind foundFromInFront;
    BitVector valuesMadeOutOfWhatStandsInFront;
    auto isMadeOutOfWhatStandsInFront = [&](ValueID written) {
        ValueID value = index.valueBehindCasts(written);
        return value && (value.index() < firstValueOfLoop || valuesMadeOutOfWhatStandsInFront.get(value.index() - firstValueOfLoop));
    };
    auto readsRowFoundUnderKeyMadeOutOfWhatStandsInFront = [&](const BodyValue& read) {
        ValueID row = index.valueBehindCasts(read.objectOperand());
        if (!row || row.index() < firstValueOfLoop || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet)
            return false;
        ValueID holder = index.valueBehindCasts(index.value(row).objectOperand());
        return holder && holder.index() < firstValueOfLoop && isMadeOutOfWhatStandsInFront(index.value(row).propertyOperand());
    };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::ElemGet && (readsListFoundByReadFromInFront(program, index, operation, firstValueOfLoop, foundFromInFront) || readsRowFoundUnderKeyMadeOutOfWhatStandsInFront(operation)))
                return true;
            bool isMadeOutOfIt = operation.opcode() == Opcode::Integer || allNumericOperators.contains(operation.opcode());
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                isMadeOutOfIt = isMadeOutOfIt && isMadeOutOfWhatStandsInFront(operand);
            });
            if (isMadeOutOfIt)
                valuesMadeOutOfWhatStandsInFront.set(operation.id().index() - firstValueOfLoop);
        }
    }
    return false;
}

static ValueID valueThatIsHandedOnAs(Walker& walker, const BodyIndex& index, ValueID name, unsigned joinsLeft = 3)
{
    for (;;) {
        if (!index.isParameter(name)) {
            if (!inputOperationsThatOnlyHandOn.contains(index.opcodeOf(name)))
                return name;
            name = index.value(name).operand(0);
            continue;
        }
        if (ValueID narrowed = index.valueNarrowedInto(name)) {
            name = narrowed;
            continue;
        }
        BlockID join = index.blockOf(name);
        if (!joinsLeft || index.isLoopHead(join) || join == entryBlock)
            return name;
        uint32_t place = name.index() - index.block(join).firstParameter().index();
        ValueID arrives;
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, join)) {
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID handedOn = place < arguments.size() ? valueThatIsHandedOnAs(walker, index, arguments[place], joinsLeft - 1) : ValueID();
            if (!handedOn || (arrives && arrives != handedOn))
                return name;
            arrives = handedOn;
        }
        if (!arrives)
            return name;
        name = arrives;
    }
}

static bool whatComesRoundIsReadOff(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position)
{
    ValueID parameter = index.block(inputHead).parameter(position);
    LoopID loop = index.loopOf(inputHead);
    bool followsLink = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (position >= arguments.size())
            return false;
        ValueID comesRound = valueThatIsHandedOnAs(walker, index, arguments[position]);
        if (comesRound == parameter)
            continue;
        if (index.isParameter(comesRound) || index.opcodeOf(comesRound) != Opcode::PropGet || index.value(comesRound).isFreeName() || valueThatIsHandedOnAs(walker, index, index.value(comesRound).objectOperand()) != parameter)
            return false;
        followsLink = true;
    }
    return followsLink;
}

static ValueID nameUnderWhichTurnsReadInTwoBlocksOff(Walker& walker, const BodyIndex& index, BlockID inputHead, ValueID parameter)
{
    BlockID first;
    ValueID asWritten;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::PropGet || operation.isFreeName())
                continue;
            if (valueThatIsHandedOnAs(walker, index, operation.objectOperand()) != parameter)
                continue;
            if (first && first != member)
                return asWritten;
            first = member;
            asWritten = operation.objectOperand();
        }
    }
    return { };
}

static void appendLinksHeldWhereTheyComeRound(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<CounterTestedInFrontOfLoop, 2>& counters)
{
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    for (uint32_t position = 0; position < head.numberOfParameters() && position < entering.size(); ++position) {
        if (!entering[position] || !entering[position].isValue() || counters.containsIf([&](const CounterTestedInFrontOfLoop& counter) { return counter.position == position; }))
            continue;
        ValueID parameter = head.parameter(position);
        ValueID asWritten = nameUnderWhichTurnsReadInTwoBlocksOff(walker, index, inputHead, parameter);
        if (!asWritten || !whatComesRoundIsReadOff(walker, index, inputHead, position))
            continue;
        TypeID expected = findExpectation(walker, parameter).type;
        if (!expected)
            expected = findExpectation(walker, asWritten).type;
        if (!expected)
            continue;
        Type type = types.at(expected);
        if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || !holdsOneMember(type.layouts) || (type.primitives & ~Primitive::UndefinedOrNull))
            continue;
        if (!types.overlap(walker.typeOf(entering[position]), types.withoutNullish(expected)))
            continue;
        TypeID held = types.unionOf(expected, TypeArena::fromPrimitives(Primitive::UndefinedOrNull));
        if (position < preview.typesOfParameters.size() && preview.typesOfParameters[position] && types.isSubset(preview.typesOfParameters[position], held))
            continue;
        counters.append({ position, types.withoutNullish(expected), true, true });
    }
}

bool Walker::buildLoopTwice(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    if (!m_buysTwoBuilds && !m_asksOnlyWhetherLoopMayBeBuiltTwice)
        return false;
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    if (!m_otherCopiesInsideTypedCopies.isEmpty() && m_otherCopiesInsideTypedCopies.last() != std::pair { frameIndex, inputHead } && index.loop(loop).numberOfLoopsWithin > 1)
        return false;
    ClosedCopyShared* shared = !m_closedCopiesShared.isEmpty() && m_closedCopiesShared.last().frameIndex == frameIndex && m_closedCopiesShared.last().inputHead == inputHead && (m_asksOnlyWhetherLoopMayBeBuiltTwice ? m_closedCopiesShared.last().isAboutToBeTaken : std::exchange(m_closedCopiesShared.last().isAboutToBeTaken, false)) ? &m_closedCopiesShared.last() : nullptr;
    if (!shared && m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop))
        return false;
    bool frontIsEnteredOutOfTurns = shared && shared->isEnteredOutOfTurns;
    BlockID sharedStart = shared && !frontIsEnteredOutOfTurns ? shared->start : BlockID();
    BlockID sharedEntry = shared ? shared->entry : BlockID();
    bool sharedCopyIsPlainWalk = shared && shared->isPlainWalk;
    bool frontLeadsToNextStartOfMember = shared && shared->otherHoldersAreAskedForLists;
    if (sharedCopyIsPlainWalk)
        m_closedCopiesShared.removeLast();

    auto forgetOperatorsMadeInFront = makeScopeExit([this, kept = m_operatorsMadeInFrontOfLoops.size()] {
        m_operatorsMadeInFrontOfLoops.shrink(kept);
    });
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
    bool stepsWhatIsNoEnumeration = steppedAtHead.asTheyEntered.containsIf([](const auto& entered) { return entered.second.otherForm == IterationByIndex::OtherForm::None && entered.second.contents != IterationByIndex::Contents::View && entered.second.contents != IterationByIndex::Contents::Text; });
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
    appendLinksHeldWhereTheyComeRound(*this, inputHead, preview, entering, counters);
    Vector<VariableAskedInFrontOfLoop, 2> variablesAskedInFront;
    Vector<VariableID, 2> functionsOfOtherFilesAskedInFront;
    guessDivisorsOfIntegersInFrontOfLoop(*this, inputHead, preview, entering, guesses, variablesAskedInFront);
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::TypeTest && operation.testedType() == TypeTable::absent) {
                ValueID asked = operation.operand(0);
                if (index.isParameter(asked) || index.opcodeOf(asked) != Opcode::GetModuleVariable)
                    continue;
                VariableID place = index.value(asked).variable();
                TypeID seen = facts().held().variable(place).proven;
                TypeID objects = seen && !holdsOnlyPrimitives(seen) ? m_types.withOnlyObjects(seen) : TypeID();
                if (objects && singleLayoutOf(m_types, m_types.at(objects)) && !variablesAskedInFront.containsIf([&](const VariableAskedInFrontOfLoop& other) { return other.variable == place; }))
                    variablesAskedInFront.append({ place, objects, StatisticID::VariablesAskedWhetherTheyAreThereReadInFrontOfLoops });
                continue;
            }
            if (operation.opcode() == Opcode::Call) {
                ValueID called = index.valueBehindCasts(operation.calleeOperand());
                if (called && !index.isParameter(called) && index.opcodeOf(called) == Opcode::CheckInitialized)
                    called = index.valueBehindCasts(index.value(called).operand(0));
                if (!called || index.isParameter(called) || index.opcodeOf(called) != Opcode::GetModuleVariable)
                    continue;
                VariableID place = index.value(called).variable();
                TypeID closures = facts().linked().variables[place.index()].isFilled ? fewClosuresWhoseCopiesPayInLoop(*this, facts().held().variable(place).proven, operation.argumentsOperands().size()) : TypeID();
                TypeID proven = m_positionsOfBuild->variable(TypeLevel::Proven, place);
                if (closures && !(proven && m_types.isSubset(proven, closures)) && !variablesAskedInFront.containsIf([&](const VariableAskedInFrontOfLoop& other) { return other.variable == place; }) && index.loopOf(member) == loop && BlocksThatEveryTurnPasses(index, loop).contains(member)) {
                    variablesAskedInFront.append({ place, closures, StatisticID::FewClosuresInVariablesReadInFrontOfLoops });
                    if (program().at(place).module != program().at(currentFunction()).module)
                        functionsOfOtherFilesAskedInFront.append(place);
                }
                continue;
            }
            if (operation.opcode() != Opcode::GetModuleVariable)
                continue;
            VariableID variable = operation.variable();
            if (program().at(variable).module == program().at(currentFunction()).module || !facts().linked().variables[variable.index()].isFilled)
                continue;
            if (m_state.isPastDeadZone(variable) || plan().deferredDefinitions().findDeferred(variable))
                continue;
            TypeID held = m_positionsOfBuild->variable(TypeLevel::Proven, variable);
            if (!held || !m_types.at(held).isObject() || !singleFunctionOf(m_types, m_types.at(held)))
                continue;
            if (!variablesAskedInFront.containsIf([&](const VariableAskedInFrontOfLoop& asked) { return asked.variable == variable; }))
                variablesAskedInFront.append({ variable, held, StatisticID::FunctionsOfOtherFilesReadInFrontOfLoops }), functionsOfOtherFilesAskedInFront.append(variable);
        }
    }
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        bool isStepOfText = entered.contents == IterationByIndex::Contents::Text;
        bool isAskedInFront = entered.mayGoByProtocol && (isStepOfText || entered.contents == IterationByIndex::Contents::View) && entered.isDeadBehindLoop;
        if (isAskedInFront && !guesses.containsIf([&](const GuessInFrontOfLoop& asked) { return asked.value == entered.list; }))
            guesses.append(GuessInFrontOfLoop { entered.list, isStepOfText ? TypeID(TypeTable::string) : m_types.builtinClass(entered.builtinClass), { }, { } });
    }
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        if (entered.mayGoByProtocol && !guesses.containsIf([&](const GuessInFrontOfLoop& asked) { return asked.value == entered.list; }))
            return false;
    }
    Vector<std::pair<Reference, TypeID>, 4> testedInFront;
    for (const GuessInFrontOfLoop& guess : guesses)
        testedInFront.append({ guess.value, guess.tested });
    bool asksAtHead = !!findReadAskedAtHeadOfLoop(inputHead, testedInFront.span());
    std::optional<ListTestedInFrontOfLoop> listAskedAtHead;
    if (m_listsAreStruckInRow && !asksAtHead && (!shared || sharedCopyIsPlainWalk) && steppedAtHead.asTheyEntered.isEmpty() && has(Mechanism::TypedOperations) && !hasTextsInStringBuilders()) {
        Vector<GuessInFrontOfLoop, 4> guessesBesideLists = guesses;
        SetForScope strikeIsLifted(m_listsAreStruckInRow, false);
        ListsReadInLoop struck = findListsReadInLoop(*this, inputHead, preview, entering, guessesBesideLists);
        listAskedAtHead = findListThatHeadReadsUnderKeyFromInFront(inputHead, struck.lists.span());
    }
    asksAtHead |= !!listAskedAtHead;
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
    for (ListTestedInFrontOfLoop& list : listsRead.lists) {
        if (!list.kindMayWellBeAnother || (!m_walksCopyForAnotherKindOfStorage && (!shared || sharedCopyIsPlainWalk) && closedCopyHasEntry && steppedAtHead.asTheyEntered.isEmpty() && !m_numberOfCopiesBuiltForSize && !m_walksCopyWithoutGuessesOfSomeTurns))
            continue;
        if (list.keepsKindWhereNoCopyIsWalkedForAnother) {
            list.kindMayWellBeAnother = false;
            continue;
        }
        list.storage = list.kindAskedOtherwise;
        list.admitsInt32Storage = list.storage != ArrayStorage::Other;
        list.kindMayWellBeAnother = false;
    }
    bool otherCopyAsksCountersAlone = listsRead.someListIsTakenUnderCounterAskedInFront && !m_walksCopyThatAsksCountersAlone && !m_asksOnlyWhetherLoopMayBeBuiltTwice;
    if (otherCopyAsksCountersAlone && (sharedCopyIsPlainWalk || !closedCopyHasEntry || !steppedAtHead.asTheyEntered.isEmpty() || loopCarriesValuesOfConsumerRound(inputHead)))
        return false;
    bool closedCopyIsSharedBehindOtherCopy = otherCopyAsksCountersAlone && sharedEntry;
    bool otherCopyBoughtNothing = false;
    bool frontLeadsOnAsItDid = closedCopyIsSharedBehindOtherCopy && frontLeadsToNextStartOfMember;
    if (closedCopyIsSharedBehindOtherCopy)
        sharedEntry = { };
    if (closedCopyIsSharedBehindOtherCopy && !frontLeadsOnAsItDid)
        sharedStart = { };
    auto isHeldWhereItComesRound = [](const CounterTestedInFrontOfLoop& counter) {
        return counter.isHeldWhereItComesRound;
    };
    bool closedCopyTakesWhatComesRound = closedCopyHasEntry && steppedAtHead.asTheyEntered.isEmpty();
    if (closedCopyTakesWhatComesRound && counters.containsIf(isHeldWhereItComesRound) && mayKeepTextsInStringBuilders(*this) && !preview.effects.maySuspend) {
        Vector<GrowingText, 2> texts;
        findGrowingTexts(*this, inputHead, preview, texts);
        closedCopyTakesWhatComesRound = texts.isEmpty();
    }
    bool sumsAreAskedInFrontAlone = false;
    if (!closedCopyTakesWhatComesRound) {
        bool sumsMayStay = closedCopyHasEntry && !steppedAtHead.asTheyEntered.isEmpty();
        counters.removeAllMatching([&](const CounterTestedInFrontOfLoop& counter) {
            return counter.isHeldWhereItComesRound && !(sumsMayStay && counter.isSum && entering[counter.position] && !m_types.isSubset(typeOf(entering[counter.position]), counter.tested));
        });
        for (CounterTestedInFrontOfLoop& counter : counters) {
            sumsAreAskedInFrontAlone |= counter.isHeldWhereItComesRound;
            counter.isHeldWhereItComesRound = false;
        }
    }
    auto isNoSum = [](const CounterTestedInFrontOfLoop& counter) { return !counter.isSum; };
    bool hasTestInFront = !guesses.isEmpty() || !listsRead.lists.isEmpty() || counters.containsIf(isNoSum) || asksAtHead;
    bool storesIntoListsTestedInFront = listsRead.lists.containsIf([](const ListTestedInFrontOfLoop& list) {
        return list.isWritable;
    });
    PlacesOfListsByName placesOfListsRead;
    bool guessesAtNothingButListsTestedInFront = !guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
        return !hasKind(m_types, guess.tested, OperandKind::Array) || placesOfListsRead.find(listsRead.lists.span(), guess.value) == notFound;
    });
    bool guessesOnlyAtListsTestedInFront = storesIntoListsTestedInFront && guessesAtNothingButListsTestedInFront;
    auto testMayWellFail = [](const CounterTestedInFrontOfLoop& counter) { return counter.testMayWellFail; };
    bool otherCopyOpensItsGuesses = std::ranges::all_of(counters, testMayWellFail) && !asksAtHead;
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
    bool guardsNotedAreOnlyTestInFront = !hasTestInFront && !m_guardsOfStoresAskedOfRow.isEmpty();
    for (const MemberConditionRecord& condition : m_guardsOfStoresAskedOfRow) {
        if (!guardsAskedOfObjectsOfTurns.contains(condition))
            guardsAskedOfObjectsOfTurns.append(condition);
    }
    hasTestInFront |= !guardsAskedOfObjectsOfTurns.isEmpty();
    auto typeInRow = [&](ValueID input) -> TypeID {
        TypeID known;
        if (input.index() >= preview.firstValue && input.index() - preview.firstValue < preview.typesOfValues.size())
            known = preview.typesOfValues[input.index() - preview.firstValue];
        else if (Reference built = referenceOf(input))
            known = typeOf(built);
        return known ? known : TypeID(TypeTable::anything);
    };
    auto asksItsSiteAlone = [&](const BodyValue& operation) {
        if (!siteMayAnswerReadWithoutOtherCode(*this, operation))
            return false;
        TypeID holder = typeInRow(operation.objectOperand());
        return m_types.saysNothing(holder) || (m_types.at(holder).isObject() && (m_types.at(holder).hasOnlyClosures() || m_types.at(holder).anyObject));
    };
    auto everyOperationThatRanOtherCodeMayBeTyped = [&](bool countsOnListsFoundFromInFront) {
        if (m_operationsThatRanOtherCodeOfRow.isEmpty())
            return false;
        WhatReadsFromInFrontOfLoopFind foundFromInFront;
        bool sitesMayAnswer = !m_someCallStaysInTurnOfRow && someReadThatAsksItsSiteIsReckonedWith(*this, m_operationsThatRanOtherCodeOfRow.span(), TypesOfInputs(typeInRow));
        bool someListIsRead = !listsRead.lists.isEmpty() || (countsOnListsFoundFromInFront && std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            return ran && readsListFoundByReadFromInFront(program(), index, index.value(ran), index.block(inputHead).firstParameter().index(), foundFromInFront);
        }));
        SetForScope purchaseAsks(m_purchaseAsksWhetherTurnsCanBeQuiet, std::ranges::none_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            return ran && effectRuleOf(index.value(ran).opcode()) == EffectRule::Call;
        }));
        for (ValueID ran : m_operationsThatRanOtherCodeOfRow) {
            if (!ran || !index.isInLoop(index.blockOf(ran), loop))
                return false;
            BodyValue operation = index.value(ran);
            if (false)
                return false;
            bool storesIntoListTested = operation.opcode() == Opcode::ElemSet && storesIntoListTestedInFrontOfLoop(*this, index, operation, index.block(inputHead).firstParameter().index(), listsRead.lists.span());
            bool readsListTested = !listsRead.lists.isEmpty() && (operation.opcode() == Opcode::ElemGet || storesIntoListTested || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == program().keyOf(BuiltinNames::length)) || findListSteppedByIndexAlone(*this, index, operation));
            readsListTested |= stepsViewByIndexAlone(*this, index, operation);
            readsListTested |= stepsTextByIndexAlone(*this, index, operation);
            readsListTested |= countsOnListsFoundFromInFront && readsListFoundByReadFromInFront(program(), index, operation, index.block(inputHead).firstParameter().index(), foundFromInFront);
            if (!readsListTested && effectRuleOf(operation.opcode()) != EffectRule::Operator && !guessesMayTypeOperationInCleanTurn(*this, operation, TypesOfInputs(typeInRow), sitesMayAnswer))
                return false;
        }
        return true;
    };
    std::unique_ptr<LoopPreview> ofTypedCopy;
    bool turnsAreQuietByRow = m_rowSaysTurnsAreQuietOutsideArms || everyOperationThatRanOtherCodeMayBeTyped(false) || m_firstOtherCodeRanInCopyOfRow;
    bool turnsAreQuietBehindListsFoundFromInFrontAlone = !turnsAreQuietByRow && everyOperationThatRanOtherCodeMayBeTyped(true);
    if (turnsOfTypedCopyMayBeginAgain(inputHead, preview) && (turnsAreQuietByRow || turnsAreQuietBehindListsFoundFromInFrontAlone)) {
        ofTypedCopy = makeUniqueWithoutFastMallocCheck<LoopPreview>();
        ofTypedCopy->effects = m_effectsOutsideArmsOfRow;
        ofTypedCopy->typesOfParameters = preview.typesOfParameters;
        ofTypedCopy->typesOfValues = preview.typesOfValues;
        ofTypedCopy->firstValue = preview.firstValue;
        ofTypedCopy->guessesCountedOn = m_testsSeenOfRow;
        for (ValueID ran : m_operationsThatRanOtherCodeOfRow) {
            if (!ran || index.opcodeOf(ran) != Opcode::PropGet)
                continue;
            BodyValue read = index.value(ran);
            ValueID receiver = read.objectOperand();
            if (read.isFreeName() || !singleBuiltinClassOf(m_types, m_types.at(typeInRow(receiver))) || ofTypedCopy->guessesCountedOn.containsIf([&](const GuessCountedOnByLooks& seen) { return seen.operand == receiver; }))
                continue;
            ofTypedCopy->guessesCountedOn.append(GuessCountedOnByLooks { ran, receiver, typeInRow(receiver) });
        }
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
    bool turnsThatRunOtherCodeComeRound = typedCopyIsQuietByRow && std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
        return wayBehindOperationComesRound(inputHead, ran);
    });
    Effects effectsOfTypedCopyByRow = m_effectsOutsideArmsOfRow;
    effectsOfTypedCopyByRow.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
    Vector<FieldOfScope, 4> fieldsTestedInFront;
    if (closedCopyHasEntry && !sharedEntry && steppedAtHead.asTheyEntered.isEmpty())
        findFieldsOfScopesFromOutsideThatQuietTurnsStore(inputHead, ofTypedCopy ? ofTypedCopy->effects : preview.effects, fieldsTestedInFront, false);
    auto staysInt32ByRow = [&](const FieldOfScope& field) {
        TypeID read = resultTypeOf(draftOfReadOf(field));
        return field.isSteppedOrAddedTo && (!field.isReadBySumsAlone || field.isSteppedByOne) && !m_types.isSubset(read, TypeTable::int32) && m_types.isSubset(TypeTable::int32, read) && std::ranges::all_of(field.addedInTurns, [&](ValueID added) {
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
    for (const VariableAskedInFrontOfLoop& asked : variablesAskedInFront) {
        if (!closedCopyHasEntry || sharedEntry || !steppedAtHead.asTheyEntered.isEmpty())
            break;
        FieldOfScope field;
        field.variable = asked.variable;
        field.type = asked.tested;
        fieldsTestedInFront.append(WTF::move(field));
    }
    hasTestInFront |= !fieldsTestedInFront.isEmpty();
    guardsNotedAreOnlyTestInFront &= fieldsTestedInFront.isEmpty();
    otherCopyOpensItsGuesses &= fieldsTestedInFront.isEmpty() || (counters.containsIf(testMayWellFail) && std::ranges::all_of(fieldsTestedInFront, [&](const FieldOfScope& field) {
        return field.variable && functionsOfOtherFilesAskedInFront.contains(field.variable);
    }));
    CounterHeldBelowRoomOfViews heldBelowRoom;
    bool storesUnderCounterAloneRanOtherCode = false;
    if (const Effects& ofTurns = ofTypedCopy ? ofTypedCopy->effects : preview.effects; closedCopyHasEntry && steppedAtHead.asTheyEntered.isEmpty() && !asksAtHead && !m_numberOfCopiesBuiltForSize
        && has(Mechanism::TypedOperations) && has(Mechanism::UnboxedNumbers) && !isUnderHandler() && !ofTurns.maySuspend) {
        bool viewsMayBeHeld = ofTurns.mayRunOtherCode() ? !ofTypedCopy && !m_operationsThatRanOtherCodeOfRow.isEmpty() : !ofTurns.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }));
        auto isListOnlyReadInLoop = [&](ValueID written) {
            Reference built = referenceOf(written);
            if (!built || !built.isValue())
                return false;
            auto isOnlyRead = [](const ListTestedInFrontOfLoop& list) {
                return hasArrayStorageForm(list.storage) && !list.isWritable && !list.turnsStoreIntoLists;
            };
            if (std::optional<ListTestedInFrontOfLoop> testedFurtherOut = findListTestedInFrontOfLoop(*this, built))
                return isOnlyRead(*testedFurtherOut);
            return listsRead.lists.containsIf([&](const ListTestedInFrontOfLoop& list) {
                return list.name == built && isOnlyRead(list);
            });
        };
        auto typeThatEnters = [&](uint32_t position) -> TypeID {
            for (const CounterTestedInFrontOfLoop& counter : counters) {
                if (counter.position == position && !counter.testMayWellFail)
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
        if (viewsMayBeHeld || !listsRead.lists.isEmpty() || m_headOfTypedBuildOverListsTestedInFront)
            heldBelowRoom = findCounterHeldBelowRoomOfViews(*this, inputHead, scopedLambda<TypeID(uint32_t)>(typeThatEnters), scopedLambda<TypeID(ValueID)>(typeInFront), scopedLambda<bool(ValueID)>(isListOnlyReadInLoop));
        bool holdsViews = heldBelowRoom.viewWhoseLengthIsBound || heldBelowRoom.accesses.containsIf([](const AccessUnderCounterOfLoop& access) { return !access.isOfList; });
        if (holdsViews) {
            heldBelowRoom.accesses.removeAllMatching([](const AccessUnderCounterOfLoop& access) { return access.isOfList; });
            if (!viewsMayBeHeld)
                heldBelowRoom.accesses.shrink(0);
        }
        if (holdsViews && ofTurns.mayRunOtherCode() && !heldBelowRoom.accesses.isEmpty()) {
            BitVector isUnderCounter;
            for (const AccessUnderCounterOfLoop& access : heldBelowRoom.accesses)
                isUnderCounter.set(access.access.index());
            storesUnderCounterAloneRanOtherCode = std::ranges::all_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
                if (!ran || !isUnderCounter.get(ran.index()) || index.opcodeOf(ran) != Opcode::ElemSet)
                    return false;
                ValueID stored = index.valueBehindCasts(index.value(ran).valueOperand());
                return isUnderCounter.get(stored.index()) && !index.isParameter(stored) && index.opcodeOf(stored) == Opcode::ElemGet;
            });
            if (!storesUnderCounterAloneRanOtherCode)
                heldBelowRoom.accesses.shrink(0);
        }
    }
    bool holdsCounterBelowRoom = !heldBelowRoom.accesses.isEmpty();
    bool headMayAskLesserOfBoundAndGreatestInt32 = !holdsCounterBelowRoom && closedCopyHasEntry && steppedAtHead.asTheyEntered.isEmpty() && !asksAtHead && !m_numberOfCopiesBuiltForSize && !isUnderHandler() && !(ofTypedCopy ? ofTypedCopy->effects : preview.effects).maySuspend;
    RelationOfHeadOnNumber relationOfHeadOnNumber;
    if (headMayAskLesserOfBoundAndGreatestInt32)
        relationOfHeadOnNumber = findRelationOfHeadBetweenCounterThatEntersAsInt32AndNumber(*this, inputHead, ofTypedCopy && !turnsThatRunOtherCodeComeRound ? ofTypedCopy->effects : preview.effects, entering, counters.span());
    auto boundOfRelationFromInFront = [&] {
        Reference made = nameOfOperatorMadeInFrontOfLoop(relationOfHeadOnNumber.number);
        return made ? made : referenceOf(relationOfHeadOnNumber.number);
    };
    auto asksWhetherBoundOfHeadIsNumber = [&](const GuessInFrontOfLoop& guess) {
        return relationOfHeadOnNumber.relation && !relationOfHeadOnNumber.variable && guess.value == boundOfRelationFromInFront() && m_types.at(guess.tested).isNumber();
    };
    bool someReadAsksItsSite = std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
        return ran && asksItsSiteAlone(index.value(ran));
    }) && !m_someCallStaysInTurnOfRow;
    bool countsOnShapeOfWhatTurnMakes = ofTypedCopy && std::ranges::any_of(ofTypedCopy->guessesCountedOn, [&](const GuessCountedOnByLooks& seen) {
        return seen.operand.index() >= preview.firstValue && seen.expected && m_types.at(seen.expected).hasObjects() && m_types.saysNothing(typeInRow(seen.operand));
    });
    bool leavesTypedCopyAtOddValues = someReadAsksItsSite || countsOnShapeOfWhatTurnMakes || turnsReadRowsOfNumbersThatTheyFind(*this, inputHead, listsRead.lists.span(), guesses.span());
    otherCopyOpensItsGuesses &= guessesOnlyAtListsTestedInFront || ((leavesTypedCopyAtOddValues || sumsAreAskedInFrontAlone) && guessesAtNothingButListsTestedInFront) || std::ranges::all_of(guesses, asksWhetherBoundOfHeadIsNumber);
    auto readOfBoundOfRelation = [&] {
        FieldOfScope read;
        read.variable = relationOfHeadOnNumber.variable;
        read.readOfTextThatVouches = relationOfHeadOnNumber.number;
        return read;
    };
    auto typeOfBoundOfRelation = [&] {
        return relationOfHeadOnNumber.variable ? resultTypeOf(draftOfReadOf(readOfBoundOfRelation())) : typeOf(boundOfRelationFromInFront());
    };
    bool isBoughtForBoundAlone = relationOfHeadOnNumber.relation && (!hasTestInFront || guardsNotedAreOnlyTestInFront) && variablesCheckedInTurns.isEmpty() && !ofTypedCopy && !m_types.at(typeOfBoundOfRelation()).isNumberWithinBounds();
    hasTestInFront |= isBoughtForBoundAlone;
    bool hasNothingToAskInFront = !hasTestInFront && variablesCheckedInTurns.isEmpty() && !ofTypedCopy && !holdsCounterBelowRoom;
    bool isBoughtForListsFoundFromInFront = hasNothingToAskInFront && closedCopyHasEntry && !preview.effects.mayRunOtherCode() && (turnsReadListsFoundFromInFront(program(), index, inputHead) || m_turnsOfRowTakeApartListsFromInFront || (m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces && !preview.effects.writes.overlaps(LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }))));
    if (hasNothingToAskInFront && !isBoughtForListsFoundFromInFront) {
        if (counters.isEmpty())
            return false;
        hasTestInFront = true;
    }
    otherCopyOpensItsGuesses &= variablesCheckedInTurns.isEmpty() || ofTypedCopy || !listsRead.lists.isEmpty();
    otherCopyOpensItsGuesses &= !ofTypedCopy || !ofTypedCopy->countsOnMembersAbovePrimitives || !ofTypedCopy->guessesCountedOn.isEmpty();
    otherCopyOpensItsGuesses |= steppedAtHead.asTheyEntered.containsIf([&](const auto& stepped) {
        return stepped.second.textOrListMayStandInstead && guesses.containsIf([&](const GuessInFrontOfLoop& guess) { return guess.value == stepped.second.list; });
    });
    if (!otherCopyOpensItsGuesses && counters.containsIf(testMayWellFail)) {
        counters.removeAllMatching(testMayWellFail);
        hasTestInFront = !guesses.isEmpty() || !listsRead.lists.isEmpty() || !counters.isEmpty() || asksAtHead || !guardsAskedOfObjectsOfTurns.isEmpty() || !fieldsTestedInFront.isEmpty();
        if (!hasTestInFront && variablesCheckedInTurns.isEmpty() && !ofTypedCopy && !holdsCounterBelowRoom)
            return false;
    }
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
    if (headMayAskLesserOfBoundAndGreatestInt32 && ofTypedCopy && !relationOfHeadOnNumber.relation)
        relationOfHeadOnNumber = findRelationOfHeadBetweenCounterThatEntersAsInt32AndNumber(*this, inputHead, turnsThatRunOtherCodeComeRound ? preview.effects : ofTypedCopy->effects, entering, counters.span());
    if ((!sharedEntry || sharedCopyIsPlainWalk) && !hasTestInFront && variablesCheckedInTurns.isEmpty() && !counters.containsIf(isNoSum) && !holdsCounterBelowRoom && !isBoughtForListsFoundFromInFront) {
        uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
        const Effects& ofTurns = ofTypedCopy->effects;
        BitVector callsFoundTheSame;
        auto isTheSameInEveryTurn = [&](auto& self, ValueID written, unsigned depth) -> bool {
            ValueID value = index.valueBehindCasts(written);
            if (value.index() < firstValueOfLoop)
                return true;
            if (!depth || index.isParameter(value))
                return false;
            BodyValue definition = index.value(value);
            if (definition.opcode() == Opcode::Builtin || definition.opcode() == Opcode::String)
                return true;
            if (definition.opcode() == Opcode::Call) {
                if (callsFoundTheSame.get(value.index() - firstValueOfLoop))
                    return true;
                bool takesTheSame = true;
                definition.forEachOperand([&](ValueID operand, uint32_t) {
                    takesTheSame = takesTheSame && self(self, operand, depth - 1);
                });
                if (takesTheSame)
                    callsFoundTheSame.set(value.index() - firstValueOfLoop);
                return takesTheSame;
            }
            if (definition.opcode() == Opcode::CheckInitialized)
                return self(self, definition.operand(0), depth - 1);
            if (definition.opcode() == Opcode::GetModuleVariable)
                return !declaredEffectsOfOperation(program(), Opcode::GetModuleVariable, definition).reads.overlaps(ofTurns.writes);
            if (definition.opcode() == Opcode::GetEnvironmentField)
                return !declaredEffectsOfOperation(program(), Opcode::GetEnvironmentField, definition).reads.overlaps(ofTurns.writes) && self(self, definition.environmentOperand(), depth - 1);
            if (definition.opcode() == Opcode::ElemGet)
                return !declaredBoundInTable(Opcode::GetOwnValueUnderKeyOrAbsent).reads.overlaps(ofTurns.writes) && self(self, definition.objectOperand(), depth - 1) && self(self, definition.propertyOperand(), depth - 1);
            if (definition.opcode() != Opcode::PrivateGet && (definition.opcode() != Opcode::PropGet || definition.isFreeName()))
                return false;
            LocationMasks slot;
            slot.add(program(), MemoryLocation::objectField(keyOfAccessToSlot(program(), definition)));
            if (slot.overlaps(ofTurns.writes))
                return false;
            return self(self, definition.objectOperand(), depth - 1);
        };
        BlocksThatEveryTurnPasses passedByEveryTurn(index, index.loopOf(inputHead));
        bool somethingLeavesTheTurns = ofTypedCopy->countsOnMembersAbovePrimitives || ofTypedCopy->guessesCountedOn.containsIf([&](const GuessCountedOnByLooks& guess) {
            return isTheSameInEveryTurn(isTheSameInEveryTurn, guess.operand, 4);
        }) || (someReadAsksItsSite && std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            return ran && asksItsSiteAlone(index.value(ran)) && passedByEveryTurn.contains(index.blockOf(ran)) && isTheSameInEveryTurn(isTheSameInEveryTurn, ran, 4);
        })) || std::ranges::any_of(m_operationsThatRanOtherCodeOfRow, [&](ValueID ran) {
            if (!ran || !passedByEveryTurn.contains(index.blockOf(ran)))
                return false;
            BodyValue read = index.value(ran);
            bool readsList = read.opcode() == Opcode::ElemGet || (read.opcode() == Opcode::PropGet && !read.isFreeName() && read.key() == program().keyOf(BuiltinNames::length));
            return readsList && m_types.saysNothing(typeInRow(read.objectOperand())) && isTheSameInEveryTurn(isTheSameInEveryTurn, read.objectOperand(), 4);
        });
        auto someLookupInMapLeavesTheTurns = [&] {
            if (m_someCallStaysInTurnOfRow)
                return false;
            for (BlockID member : index.membersOf(loop)) {
                if (!passedByEveryTurn.contains(member))
                    continue;
                for (BodyValue operation : index.block(member).values()) {
                    if (!lookupInMapIsOnlyReckonedWithAsNumber(*this, operation, TypesOfInputs(typeInRow)) || !isTheSameInEveryTurn(isTheSameInEveryTurn, operation.thisValueOperand(), 4))
                        continue;
                    ValueID key = index.valueBehindCasts(operation.argumentsOperands()[0]);
                    if ((!index.isParameter(key) && index.opcodeOf(key) == Opcode::String) || isTheSameInEveryTurn(isTheSameInEveryTurn, key, 4))
                        return true;
                }
            }
            return false;
        };
        if (!somethingLeavesTheTurns && !m_firstOtherCodeRanInCopyOfRow && !someLookupInMapLeavesTheTurns())
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
    bool likelyWayLeadsToClosedStart = false;
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
    if ((sharedCopyIsPlainWalk ? !checksVariableInFront : closedStart == sharedStart) && !hasTestInFront && !ofTypedCopy && !holdsCounterBelowRoom && !isBoughtForListsFoundFromInFront)
        return false;
    letGuessFailUnasked(closedStart, origin);
    if (failsGuesses()) [[unlikely]] {
        rareWayLeadsToClosedStart = true;
        hasTestInFront = true;
    }
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
            noteGuardsShownByTest(*this, guardsAskedOfObjectsOfTurns.span(), holds);
        }
    }
    Reference boundRoundedInFront;
    Reference lesserOfBoundAndGreatestInt32;
    Reference readOfVariableThatIsBound;
    auto askBoundOfCounter = [&] {
        Reference bound = relationOfHeadOnNumber.variable ? valueKnownOf(readOfBoundOfRelation()) : boundOfRelationFromInFront();
        if (!bound) {
            Draft inFront = draftOfReadInFrontOfHead(readOfBoundOfRelation(), ReadInFrontOfHead::IsTestedInFront);
            inFront.setFollowsItsStore(false);
            bound = build(WTF::move(inFront));
        }
        if (!bound || !bound.isValue())
            return false;
        bool isNumberByType = m_types.at(typeOf(bound)).isNumber();
        BlockFrequency whereItIsRounded = isNumberByType ? BlockFrequency::Normal : BlockFrequency::Rare;
        auto round = [&](Reference number) {
            Draft rounding(Opcode::IntegerBound, origin);
            chooseConservativeFields(rounding);
            rounding.setNumberOperand(number);
            Opcode asked = index.opcodeOf(relationOfHeadOnNumber.relation);
            rounding.setComparison(!roundsBoundsOfCountersTheOtherWay() ? asked : asked == Opcode::Less ? Opcode::LessEq : Opcode::Less);
            rounding.setNumberIsOnLeft(false);
            return build(WTF::move(rounding));
        };
        Reference rounded = isNumberByType ? round(bound) : Reference();
        if (isNumberByType && (!rounded || !rounded.isValue()))
            return false;
        Reference isInt32 = buildTestOfNumber(*this, isNumberByType ? rounded : bound, TypeTable::int32, origin);
        if (!isInt32 || !isInt32.isValue())
            return false;
        if (!closedStart)
            closedStart = addBlock(BlockFrequency::Rare);
        rareWayLeadsToClosedStart = true;
        auto goOnWhere = [&](Reference test) {
            if (!test || !test.isValue() || isUnreachable())
                return false;
            BlockID passed = addBlock(BlockFrequency::Rare);
            branch(test, passed, closedStart);
            continueIn(passed);
            return true;
        };
        Vector<ArmThatMeets, 3> arms;
        Reference number = bound;
        if (!isNumberByType) {
            BlockID takesBound = addBlock();
            BlockID isNoInt32 = addBlock(BlockFrequency::Rare);
            branch(isInt32, takesBound, isNoInt32);
            continueIn(takesBound);
            if (Reference held = buildProofOfNumber(*this, bound, isInt32, origin))
                arms.append(ArmThatMeets { takesBound, { held, held } });
            else if (!isUnreachable())
                jumpTo(closedStart);
            continueIn(isNoInt32);
            Reference isNumber = buildTestOfNumber(*this, bound, TypeTable::number, origin);
            number = goOnWhere(isNumber) ? buildProofOfNumber(*this, bound, isNumber, origin) : Reference();
        }
        if (!isNumberByType && number && !isUnreachable())
            rounded = round(number);
        Reference roundedIsInt32 = isNumberByType ? isInt32 : rounded && rounded.isValue() && !isUnreachable() ? buildTestOfNumber(*this, rounded, TypeTable::int32, origin) : Reference();
        if (roundedIsInt32 && roundedIsInt32.isValue()) {
            BlockID takesRounded = addBlock(whereItIsRounded);
            BlockID liesOutside = addBlock(BlockFrequency::Rare);
            branch(roundedIsInt32, takesRounded, liesOutside);
            continueIn(liesOutside);
            Draft written(Opcode::Integer, origin);
            written.setInteger(std::numeric_limits<int32_t>::max());
            Reference greatest = constant(WTF::move(written));
            if (goOnWhere(buildOperatorOnIntegersOfLoop(*this, Opcode::Less, greatest, rounded, origin)))
                arms.append(ArmThatMeets { currentBlock(), { greatest, rounded } });
            else if (!isUnreachable())
                jumpTo(closedStart);
            continueIn(takesRounded);
            if (Reference held = buildProofOfNumber(*this, rounded, roundedIsInt32, origin))
                arms.append(ArmThatMeets { takesRounded, { held, held } });
        } else if (!isUnreachable())
            jumpTo(closedStart);
        if (arms.isEmpty())
            return false;
        std::array<Reference, 2> met;
        if (isNumberByType) {
            for (ArmThatMeets& arm : arms)
                arm.values.shrink(1);
        }
        if (!letArmsMeet(arms.span(), addBlock(), std::span<Reference>(met).first(isNumberByType ? 1 : 2)))
            return false;
        lesserOfBoundAndGreatestInt32 = met[0];
        boundRoundedInFront = isNumberByType ? rounded : met[1];
        if (relationOfHeadOnNumber.variable) {
            ValueID read = relationOfHeadOnNumber.number;
            if (index.opcodeOf(read) == Opcode::CheckInitialized)
                read = index.valueBehindCasts(index.value(read).operand(0));
            LocationMasks restsOn;
            if (!numberReadOutOfVariableStays(*this, relationOfHeadOnNumber.variable))
                restsOn = declaredEffectsOfOperation(program(), Opcode::GetModuleVariable, index.value(read)).reads;
            noteKnown(*this, KnownNote::VariableIsAsInFrontOfLoop, bound, { }, relationOfHeadOnNumber.variable.index(), restsOn, 1, true);
            readOfVariableThatIsBound = bound;
        }
        count(StatisticID::NumbersBesideCountersAskedInFrontOfLoops);
        return true;
    };
    Vector<Reference, 4> proofs;
    for (const GuessInFrontOfLoop& guess : guesses) {
        if (asksWhetherBoundOfHeadIsNumber(guess) && !isUnreachable() && (lesserOfBoundAndGreatestInt32 || askBoundOfCounter())) {
            proofs.append(Reference());
            continue;
        }
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
        Reference proof = buildProofInFrontOfLoop(*this, guess, test, origin);
        if (proof && proof.isValue() && guess.tested == TypeTable::int32 && representationOf(proof) == Representation::Double && m_types.at(typeOf(proof)).isInt32()) {
            if (Reference word = convert(proof, Representation::Int32, origin); word && word.isValue() && word != proof) {
                Draft number(Opcode::IntegerToNumber, origin);
                number.setOperand(0, word);
                number.representation = Representation::Double;
                if (Reference held = build(WTF::move(number)))
                    proof = held;
            }
        }
        proofs.append(proof);
    }
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        size_t guess = entered.mayGoByProtocol ? guesses.findIf([&](const GuessInFrontOfLoop& asked) { return asked.value == entered.list; }) : notFound;
        if (guess != notFound && guess < proofs.size() && proofs[guess])
            m_iterationsByIndex[place].list = proofs[guess];
    }
    Vector<Reference, 8> enteringTypedCopy;
    enteringTypedCopy.append(entering);
    for (const CounterTestedInFrontOfLoop& counter : counters) {
        Reference asked = entering[counter.position];
        if (asked && counter.isHeldWhereItComesRound && m_types.isSubset(typeOf(asked), counter.tested) && (!isTypeOfLink(m_types, counter.tested) || m_state.factsOfCell(asked).contains(FactOfCell::BearsNoMark)))
            continue;
        if (!isUnreachable() && representationOf(asked) == Representation::OptionalDouble)
            asked = convert(asked, Representation::JSValue, origin);
        Reference test = isUnreachable() || !asked ? Reference() : buildTestOfCounterOrLink(*this, asked, counter.tested, origin);
        if (!test)
            continue;
        BlockID passed = addBlock();
        if (!closedStart)
            closedStart = addBlock(counter.testMayWellFail ? BlockFrequency::Normal : BlockFrequency::Rare);
        branch(test, passed, closedStart);
        (counter.testMayWellFail ? likelyWayLeadsToClosedStart : rareWayLeadsToClosedStart) = true;
        continueIn(passed);
        enteringTypedCopy[counter.position] = buildProofOfNumber(*this, asked, test, origin);
    }
    for (const FieldOfScope& field : fieldsTestedInFront) {
        bool isOfAnotherFile = field.variable && functionsOfOtherFilesAskedInFront.contains(field.variable);
        Reference read = isUnreachable() || isOfAnotherFile ? Reference() : valueKnownOf(field);
        if (!read && !isUnreachable()) {
            Draft inFront = draftOfReadInFrontOfHead(field, ReadInFrontOfHead::IsTestedInFront, isOfAnotherFile);
            size_t queued = variablesAskedInFront.findIf([&](const VariableAskedInFrontOfLoop& asked) { return asked.variable == field.variable; });
            count(queued == notFound ? StatisticID::VariablesThatQuietTurnsStoreReadInFrontOfLoops : variablesAskedInFront[queued].counted);
            read = build(WTF::move(inFront));
        }
        Reference test = read ? buildTestOfNumber(*this, read, field.type, origin) : Reference();
        if (!test || !test.isValue())
            continue;
        BlockID passed = addBlock();
        if (!closedStart)
            closedStart = addBlock(BlockFrequency::Rare);
        branch(test, passed, takesWhatLoopsAskOfVariablesInFrontToHold() ? passed : closedStart);
        continueIn(passed);
        if (isOfAnotherFile)
            holdWhatIsFoundInPlace(read, field.type, ProofSource::Variable);
        Reference proof = buildProofOfNumber(*this, read, test, origin);
        if (proof && field.variable) {
            if (program().at(field.variable).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(program(), field.variable))
                m_state.noteEndOfDeadZone(field.variable);
            m_state.noteVariable(field.variable, proof);
            noteValueOfStableBinding(*this, field.variable, proof);
        } else if (proof)
            noteEnvironmentField(field.scope, field.field, proof);
    }
    bool holdsCounterBelowLengthsOfLists = heldBelowRoom.accesses.containsIf([](const AccessUnderCounterOfLoop& access) { return access.isOfList; });
    std::optional<WalkState> knownInFrontOfTestsOfLists;
    auto buildTestsOfLists = [&] {
        if (listsRead.lists.containsIf([](const ListTestedInFrontOfLoop& list) { return list.kindMayWellBeAnother; }) && !listsRead.lists.containsIf(isConvertedInFrontOfTypedCopy))
            knownInFrontOfTestsOfLists = m_state;
        SetForScope asked(m_inputHeadAskedAboutTypedCopy, headOfLooksTriedForLists);
        buildTestsOfListsReadInLoop(*this, inputHead, entering, listsRead, guesses.span(), proofs.span(), closedStart, origin);
    };
    if (holdsCounterBelowLengthsOfLists)
        buildTestsOfLists();
    Reference lesserBound;
    Reference boundBesideLeastRoom;
    Reference viewWhoseLengthIsBound;
    WayIntoClosedCopy::CounterBelowRoomOfViews counterBelowRoom;
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
        auto goOnWhereItIsNotNegative = [&](Reference asked) -> std::optional<PassedTest> {
            if (isUnreachable() || !isInteger(asked))
                return std::nullopt;
            if (std::optional<PassedTest> byItsType = findWhatHoldsFromZeroUp(asked))
                return byItsType;
            Draft zero(Opcode::Integer, origin);
            zero.setInteger(0);
            Reference test = buildOperatorOnIntegersOfLoop(*this, Opcode::GreaterEq, asked, constant(WTF::move(zero)), origin);
            if (std::optional<PassedTest> known = findPassedTest(test))
                return known;
            if (!test || !test.isValue())
                return std::nullopt;
            if (!closedStart)
                closedStart = addBlock(BlockFrequency::Rare);
            rareWayLeadsToClosedStart = true;
            return goOnWhereTestPasses(test, closedStart);
        };
        auto testOf = [&](const PassedTest& passed) {
            return passed.sides().empty() ? ValueID() : passed.sides().front().test;
        };
        auto lengthInFrontOfLoop = [&](Reference view, bool isList = false) -> Reference {
            if (std::optional<uint64_t> noted = findKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view))
                return Reference::fromBits(static_cast<uint32_t>(*noted));
            Reference length = isUnreachable() ? Reference() : isList ? buildLengthOfListInFrontOfLoop(*this, view, origin) : buildRoomOfViewBesideAddend(*this, view, { }, origin);
            if (length && !isUnreachable())
                noteKnown(*this, KnownNote::RoomOfViewIsAsInFrontOfLoop, view, { }, 0, isList ? placesUnderLengthsOfLists : LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), length.bits(), true);
            return length;
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
        Reference bound = heldBelowRoom.viewWhoseLengthIsBound ? lengthInFrontOfLoop(builtInFront(heldBelowRoom.viewWhoseLengthIsBound)) : builtInFront(heldBelowRoom.bound);
        std::optional<PassedTest> stepsUp = !isInteger(enters) || !isInteger(bound) ? std::nullopt : heldBelowRoom.stepThatMayBeNegative ? goOnWhereItIsNotNegative(builtInFront(heldBelowRoom.stepThatMayBeNegative)) : std::optional(PassedTest());
        struct AddendsAskedOfView {
            Reference view;
            Reference fromInFront;
            Range written;
        };
        Vector<AddendsAskedOfView, 4> addendsAskedOfViews;
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
            if (!view || !view.isValue() || (addend && !isInteger(addend)) || (!isWritten && !addend))
                continue;
            if (access.isOfList ? !findListTestedInFrontOfLoop(*this, view) : !isViewOfNumbersReadInLine(*this, typeOf(view)))
                continue;
            Range written = isWritten ? writtenAddendsOfViews.get(view.bits()) : Range { 0, 0 };
            bool isAsked = addendsAskedOfViews.containsIf([&](const AddendsAskedOfView& asked) {
                if (asked.view != view)
                    return false;
                return isWritten ? !asked.fromInFront && asked.written.minimum <= written.minimum && written.maximum <= asked.written.maximum : asked.fromInFront == addend;
            });
            if (!isAsked) {
                Reference leastAddend = isWritten ? integerOf(written.minimum) : addend;
                std::optional<PassedTest> leastIsNotNegative = goOnWhereItIsNotNegative(leastAddend ? buildOperatorOnIntegersOfLoop(*this, Opcode::Add, enters, leastAddend, origin) : enters);
                if (!leastIsNotNegative)
                    continue;
                Reference room = lengthInFrontOfLoop(view, access.isOfList);
                if (room && addend && !isUnreachable())
                    room = buildOperatorOnIntegersOfLoop(*this, Opcode::Sub, room, addend, origin);
                if (!isInteger(room))
                    continue;
                Reference least = leastRoom && !isUnreachable() ? lesserOf(room, leastRoom) : room;
                if (!least || isUnreachable())
                    continue;
                leastRoom = least;
                addendsAskedOfViews.append(AddendsAskedOfView { view, isWritten ? Reference() : addend, written });
                if (ValueID test = testOf(*leastIsNotNegative); test && !counterBelowRoom.fromZeroUp.contains(test))
                    counterBelowRoom.fromZeroUp.append(test);
            }
            if (access.isOfList)
                count(StatisticID::ReadsOfListsHeldBelowLesserBound);
        }
        if (leastRoom && heldBelowRoom.viewWhoseLengthIsBound && !isUnreachable())
            viewWhoseLengthIsBound = builtInFront(heldBelowRoom.viewWhoseLengthIsBound);
        if (leastRoom && !isUnreachable() && !m_hasFailed) {
            if (index.opcodeOf(heldBelowRoom.relation) == Opcode::LessEq) {
                Draft one(Opcode::Integer, origin);
                one.setInteger(1);
                leastRoom = buildOperatorOnIntegersOfLoop(*this, Opcode::Sub, leastRoom, constant(WTF::move(one)), origin);
            }
            lesserBound = isInteger(leastRoom) ? lesserOf(bound, leastRoom) : Reference();
            boundBesideLeastRoom = bound;
            counterBelowRoom.start = enters.id();
            counterBelowRoom.position = heldBelowRoom.position;
            counterBelowRoom.stepIsFromZeroUp = testOf(*stepsUp);
        }
    }
    if (relationOfHeadOnNumber.relation && !lesserOfBoundAndGreatestInt32 && !isUnreachable())
        askBoundOfCounter();
    if (isBoughtForBoundAlone && !lesserOfBoundAndGreatestInt32) {
        if (!rareWayLeadsToClosedStart && !m_hasFailed)
            return false;
        fail("a loop bought for its bound alone asks no bound"_s);
        return true;
    }
    if (!holdsCounterBelowLengthsOfLists)
        buildTestsOfLists();
    bool testsOfListsLeadToClosedStart = false;
    if (sharedCopyIsPlainWalk && !rareWayLeadsToClosedStart) {
        m_code.forEachEdgeIntoSoFar(closedStart, [&](const Edge&) {
            testsOfListsLeadToClosedStart = true;
        });
    }
    if (m_hasFailed)
        return true;
    if (!closedStart && !asksAtHead && hasTestInFront && !counters.containsIf(isHeldWhereItComesRound)) {
        fail("a loop is built twice without a test"_s);
        return true;
    }
    noteGuessOpened();
    ++m_numberOfLoopsBuiltTwice;
    m_numberOfLoopsThatHoldLoopsBuiltTwice += index.loop(loop).numberOfLoopsWithin > 1;
    if (steppedAtHead.asTheyEntered.containsIf([](const auto& entered) { return entered.second.mayGoByProtocol && entered.second.isDeadBehindLoop && entered.second.contents == IterationByIndex::Contents::Text; }))
        count(StatisticID::LoopsOverTextsBuiltTwice);
    BlockID entryOfClosedCopy = sharedEntry;
    Vector<Reference, 8> enteringClosedCopy;
    Vector<Reference, 2> positionsEnteringClosedCopy;
    Vector<Reference, 4> carriedInFrontOfLoop;
    Vector<Reference, 4> carriedEnteringClosedCopy;
    if (!sharedEntry && closedCopyHasEntry && loopCarriesValuesOfConsumerRound(inputHead))
        carriedInFrontOfLoop = carriedValuesInFrame(frameIndex);
    if (!sharedEntry && closedCopyHasEntry) {
        entryOfClosedCopy = addBlock(otherCopyAsksCountersAlone ? BlockFrequency::Unlikely : otherCopyOpensItsGuesses ? BlockFrequency::Normal : BlockFrequency::Rare);
        m_entriesOfClosedCopies.add(entryOfClosedCopy.index());
        for (size_t index = 0; index < entering.size(); ++index) {
            TypeID type = joinTypes(preview.typesOfParameters[index], ownTypeOf(entering[index]));
            enteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
        }
        for (size_t place = 0; place < steppedAtHead.asTheyEntered.size(); ++place) {
            TypeID type = m_types.integer(Range { 0, positionBehindEveryEnd });
            positionsEnteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
        }
        for (Reference carried : carriedInFrontOfLoop) {
            size_t placeInRow = entering.size() + positionsEnteringClosedCopy.size() + carriedEnteringClosedCopy.size();
            TypeID type = ownTypeOf(carried);
            if (placeInRow < preview.typesOfParameters.size() && preview.typesOfParameters[placeInRow])
                type = joinTypes(preview.typesOfParameters[placeInRow], type);
            carriedEnteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
        }
        takeFieldsOfScopesIntoClosedCopy(entryOfClosedCopy, preview);
        settleParametersOf(entryOfClosedCopy);
    }
    takeIterationsAsTheyEntered();
    for (const auto& [place, entered] : steppedAtHead.asTheyEntered) {
        if (!otherCopyOpensItsGuesses || !entered.textOrListMayStandInstead || entered.contents != IterationByIndex::Contents::List)
            continue;
        m_iterationsByIndex[place].contents = IterationByIndex::Contents::Text;
        m_iterationsByIndex[place].builtinClass = BuiltinClasses::String;
    }

    for (ValueID value : readBehindLoop)
        m_frames[frameIndex].listAsBuiltTwice(value);
    noteValuesThatHandlersOutsideLoopRead(inputHead);
    m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
    uint32_t namesInFrontOfLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;

    SetForScope ordinalOfCopy(m_ordinalOfCopyOfLoop, m_ordinalOfCopyOfLoop * 16 + 1);
    BlockID typedStart = isUnreachable() ? BlockID() : m_block;
    WalkState stateWhereTestsPassed = m_state;
    if (frontIsEnteredOutOfTurns && !closedStart)
        closedStart = addBlock(BlockFrequency::Rare);
    WalkState stateOfClosedStartOfItsOwn;
    if (frontIsEnteredOutOfTurns)
        stateOfClosedStartOfItsOwn = stateWhereTestsPassed;
    if (!sharedEntry) {
        if (closedStart && !frontLeadsOnAsItDid)
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
                withPositions.appendVector(carriedInFrontOfLoop);
                enterWithFieldsOfScopes(entryOfClosedCopy, withPositions.span());
            }
            for (uint32_t number = 0; number < carriedEnteringClosedCopy.size(); ++number)
                bind(ValueID(index.numberOfValues() + number), carriedEnteringClosedCopy[number]);
            for (size_t place = 0; place < positionsEnteringClosedCopy.size(); ++place)
                m_iterationsByIndex[steppedAtHead.asTheyEntered[place].first].position = positionsEnteringClosedCopy[place];
            enterClosedCopy(entryOfClosedCopy, preview);
        }
        if (otherCopyAsksCountersAlone && entryOfClosedCopy) {
            count(StatisticID::LoopsWhoseOtherCopyAsksCountersAlone);
            uint32_t builtTwiceInFront = m_numberOfLoopsBuiltTwice;
            SetForScope asksCountersAlone(m_walksCopyThatAsksCountersAlone, true);
            SetForScope endedLists(m_typedBuildEndedListsTestedInFront, m_typedBuildEndedListsTestedInFront);
            SetForScope asksGuards(m_typedBuildAsksGuardsThatItWasBoughtFor, m_typedBuildAsksGuardsThatItWasBoughtFor);
            SetForScope everyTurnLeaves(m_everyTurnOfTypedBuildLeaves, m_everyTurnOfTypedBuildLeaves);
            SetForScope leavesWithoutTest(m_turnOfTypedBuildLeavesWithoutTest, m_turnOfTypedBuildLeavesWithoutTest);
            SetForScope testsNothing(m_typedBuildTestsNothing, m_typedBuildTestsNothing);
            SetForScope isLeftAtOtherCode(m_typedBuildIsLeftAtOtherCodeOnUsualWay, m_typedBuildIsLeftAtOtherCodeOnUsualWay);
            SetForScope askedNoList(m_typedBuildAskedNoListThatItWasBoughtFor, m_typedBuildAskedNoListThatItWasBoughtFor);
            SetForScope report(m_reportOfTypedBuildMadeLast, m_reportOfTypedBuildMadeLast);
            SetForScope walkWasKept(m_walkOfLoopWasKept, m_walkOfLoopWasKept);
            std::pair<uint32_t, BlockID> boughtFirst { static_cast<uint32_t>(frameIndex), inputHead };
            bool firstPurchaseIsListed = !m_loopsBeingBought.isEmpty() && m_loopsBeingBought.last() == boughtFirst;
            if (firstPurchaseIsListed)
                m_loopsBeingBought.removeLast();
            bool wayAroundBeginsLoop = !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().frameIndex == frameIndex && m_waysIntoClosedCopies.last().inputHead == inputHead;
            size_t placeOfWayAround = m_waysIntoClosedCopies.size() - wayAroundBeginsLoop;
            BlockID startOfWayAround = wayAroundBeginsLoop ? std::exchange(m_waysIntoClosedCopies[placeOfWayAround].startFromInFront, BlockID()) : BlockID();
            if (closedCopyIsSharedBehindOtherCopy) {
                m_closedCopiesShared.last().isAboutToBeTaken = true;
                m_closedCopiesShared.last().isEnteredOutOfTurns = true;
            } else {
                m_frames[frameIndex].loopsBeingBuiltTwice.removeLast();
                m_reversionableLoops.append({ frameIndex, loop });
            }
            walkCopyOfLoop(inputHead, enteringClosedCopy.span());
            if (wayAroundBeginsLoop)
                m_waysIntoClosedCopies[placeOfWayAround].startFromInFront = startOfWayAround;
            if (closedCopyIsSharedBehindOtherCopy) {
                m_closedCopiesShared.last().isAboutToBeTaken = false;
                m_closedCopiesShared.last().isEnteredOutOfTurns = false;
            } else {
                m_reversionableLoops.removeLast();
                m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
            }
            if (firstPurchaseIsListed)
                m_loopsBeingBought.append(boughtFirst);
            otherCopyBoughtNothing = !closedCopyIsSharedBehindOtherCopy && m_numberOfLoopsBuiltTwice == builtTwiceInFront;
        } else if (otherCopyOpensItsGuesses) {
            bool standsInsideTypedCopy = m_waysIntoClosedCopies.containsIf([&](const WayIntoClosedCopy& way) { return way.frameIndex != frameIndex || way.inputHead != inputHead; });
            if (standsInsideTypedCopy)
                m_otherCopiesInsideTypedCopies.append({ frameIndex, inputHead });
            walkCopyOfLoop(inputHead, entryOfClosedCopy ? enteringClosedCopy.span() : entering);
            if (standsInsideTypedCopy)
                m_otherCopiesInsideTypedCopies.removeLast();
        } else {
            ClosedArm closedArm(*this);
            ++m_numberOfCopiesBuiltForSize;
            walkCopyOfLoop(inputHead, entryOfClosedCopy ? enteringClosedCopy.span() : entering);
            --m_numberOfCopiesBuiltForSize;
        }
    }
    if (otherCopyBoughtNothing)
        m_typedBuildEndedListsTestedInFront = true;
    forgetValuesOfLoop(inputHead);
    for (uint32_t number = 0; number < carriedEnteringClosedCopy.size(); ++number)
        bind(ValueID(index.numberOfValues() + number), carriedInFrontOfLoop[number]);

    BlockID startOfCopyBeside = typedStart && entryOfClosedCopy && !lesserBound && !m_someCallStaysInTurnOfRow && !m_walksCopyWithoutGuessesOfSomeTurns && !m_hasFailed ? addBlock() : BlockID();
    WalkState stateOfCopyBeside;
    if (startOfCopyBeside)
        stateOfCopyBeside = stateWhereTestsPassed;
    bool otherCodeCameRoundByRow = turnsThatRunOtherCodeComeRound;
    bool copyBesideIsEntered = false;
    bool copyBesideIsBoughtByItself = false;
    for (bool isCopyBeside : { false, true }) {
        if (!typedStart || m_hasFailed || (isCopyBeside && !copyBesideIsEntered))
            break;
        SetForScope walksCopyBeside(m_walksCopyWithoutGuessesOfSomeTurns, m_walksCopyWithoutGuessesOfSomeTurns || isCopyBeside);
        if (isCopyBeside) {
            for (BlockID member : index.membersOf(loop))
                inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
            forgetValuesOfLoop(inputHead);
            turnsThatRunOtherCodeComeRound = otherCodeCameRoundByRow;
            count(StatisticID::LoopsBuiltOnceMoreWithoutGuessesOfSomeTurns);
        }
        m_block = isCopyBeside ? startOfCopyBeside : typedStart;
        m_state = isCopyBeside ? WTF::move(stateOfCopyBeside) : WTF::move(stateWhereTestsPassed);
        putBackIterationsSteppedAt(steppedAtHead);
        if (isCopyBeside && copyBesideIsBoughtByItself) {
            count(StatisticID::LoopsBoughtOnceMoreWithoutQuestionsOfEveryTurn);
            SetForScope endedLists(m_typedBuildEndedListsTestedInFront, m_typedBuildEndedListsTestedInFront);
            SetForScope asksGuards(m_typedBuildAsksGuardsThatItWasBoughtFor, m_typedBuildAsksGuardsThatItWasBoughtFor);
            SetForScope everyTurnLeaves(m_everyTurnOfTypedBuildLeaves, m_everyTurnOfTypedBuildLeaves);
            SetForScope leavesWithoutTest(m_turnOfTypedBuildLeavesWithoutTest, m_turnOfTypedBuildLeavesWithoutTest);
            SetForScope testsNothing(m_typedBuildTestsNothing, m_typedBuildTestsNothing);
            SetForScope isLeftAtOtherCode(m_typedBuildIsLeftAtOtherCodeOnUsualWay, m_typedBuildIsLeftAtOtherCodeOnUsualWay);
            SetForScope askedNoList(m_typedBuildAskedNoListThatItWasBoughtFor, m_typedBuildAskedNoListThatItWasBoughtFor);
            SetForScope report(m_reportOfTypedBuildMadeLast, m_reportOfTypedBuildMadeLast);
            SetForScope walkWasKept(m_walkOfLoopWasKept, m_walkOfLoopWasKept);
            SetForScope leavesOutQuestions(m_walksCopyWithoutQuestionsOfEveryTurn, true);
            m_closedCopiesShared.append(ClosedCopyShared { frameIndex, inputHead, closedStart, entryOfClosedCopy, true });
            m_waysIntoClosedCopies.append(WayIntoClosedCopy { frameIndex, inputHead, entryOfClosedCopy, { }, closedStart, { }, false });
            m_waysIntoClosedCopies.last().turnsCall = someOperationOfRowThatRanOtherCodeIsCall(index), m_waysIntoClosedCopies.last().someCallStaysInTurn = m_someCallStaysInTurnOfRow;
            m_waysIntoClosedCopies.last().reckonsWithWhatItReads = loopReckonsWithWhatItReads(inputHead);
            m_waysIntoClosedCopies.last().lastBlockThatEveryTurnPasses = BlocksThatEveryTurnPasses(index, index.loopOf(inputHead)).lastOf(index.loopOf(inputHead));
            ++m_ordinalOfCopyOfLoop;
            std::pair<uint32_t, BlockID> boughtFirst { static_cast<uint32_t>(frameIndex), inputHead };
            bool firstPurchaseIsListed = !m_loopsBeingBought.isEmpty() && m_loopsBeingBought.last() == boughtFirst;
            if (firstPurchaseIsListed)
                m_loopsBeingBought.removeLast();
            walkCopyOfLoop(inputHead, entering);
            if (firstPurchaseIsListed)
                m_loopsBeingBought.append(boughtFirst);
            dropWayIntoClosedCopy();
            m_closedCopiesShared.removeLast();
            break;
        }
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
            if (entered.contents == IterationByIndex::Contents::Text && isUnderHandler() && !isUnderHandlerOtherThanCleanups())
                flatStringMadeWhereWalkStands(*this, proofs[guess], m_origin);
        }
        Vector<std::pair<ValueID, Reference>, 8> boundBefore;
        for (size_t place = 0; place < guesses.size(); ++place) {
            if (!proofs[place])
                continue;
            for (ValueID name : guesses[place].names) {
                boundBefore.append({ name, referenceBoundTo(name) });
                bind(name, proofs[place]);
                if (isMadeInFrontOfLoop(name))
                    noteOperatorMadeInFrontOfLoop(name, proofs[place]);
            }
            if (!guesses[place].readOfVariableInLoop)
                continue;
            BodyValue read = index.value(guesses[place].readOfVariableInLoop);
            if (read.opcode() == Opcode::GetModuleVariable) {
                if (program().at(read.variable()).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(program(), read.variable()))
                    m_state.noteEndOfDeadZone(read.variable());
                m_state.noteVariable(read.variable(), proofs[place]);
                noteValueOfStableBinding(*this, read.variable(), proofs[place]);
            } else if (Reference environment = referenceOf(read.environmentOperand())) {
                FieldReference field { read.layout(), read.field() };
                m_state.noteEndOfDeadZone(environment, field);
                noteEnvironmentField(environment, field, proofs[place]);
                noteValueOfStableBinding(*this, environment, field, proofs[place]);
            }
        }
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.append(WayIntoClosedCopy { frameIndex, inputHead, entryOfClosedCopy, { }, closedStart, { }, ofTypedCopy || (listsRead.areTested && headOfLooksTriedForLists == inputHead) });
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.last().otherCopyOpensItsGuesses = otherCopyOpensItsGuesses && (!sharedEntry || sharedCopyIsPlainWalk || frontLeadsToNextStartOfMember);
        if (entryOfClosedCopy && listAskedAtHead) {
            m_waysIntoClosedCopies.last().listAskedAtHead = listAskedAtHead->name;
            m_waysIntoClosedCopies.last().storageAskedOfListAtHead = listAskedAtHead->storage;
            m_waysIntoClosedCopies.last().listAskedAtHeadIsWritable = listAskedAtHead->isWritable;
        }
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.last().reckonsWithWhatItReads = loopReckonsWithWhatItReads(inputHead);
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.last().lastBlockThatEveryTurnPasses = BlocksThatEveryTurnPasses(index, index.loopOf(inputHead)).lastOf(index.loopOf(inputHead));
        if (entryOfClosedCopy && !isCopyBeside)
            m_waysIntoClosedCopies.last().startOfCopyWithoutGuessesOfSomeTurns = startOfCopyBeside;
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.last().variablesHeld.appendVector(fieldsTestedInFront);
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.last().turnsCall = someOperationOfRowThatRanOtherCodeIsCall(index), m_waysIntoClosedCopies.last().someCallStaysInTurn = m_someCallStaysInTurnOfRow;
        bool otherCodeMayStay = turnsThatRunOtherCodeComeRound && entryOfClosedCopy && (!sharedEntry || sharedCopyIsPlainWalk) && !asksAtHead && !holdsCounterBelowRoom && listsRead.lists.isEmpty() && steppedAtHead.asTheyEntered.isEmpty()
            && std::ranges::all_of(fieldsTestedInFront, [&](const FieldOfScope& field) {
                return !field.variable || program().at(field.variable).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(program(), field.variable);
            });
        bool otherCodeStays = otherCodeMayStay && m_otherCodeStaysInTypedBuildOfRow;
        turnsThatRunOtherCodeComeRound = otherCodeStays || (otherCodeMayStay && !sharedEntry && !counters.containsIf(isNoSum) && variablesCheckedInTurns.isEmpty());
        bool otherCodeIsKeptOutOfFirstCopy = startOfCopyBeside && !isCopyBeside && !otherCodeStays && !m_otherCodeIsKeptOutOfFirstCopyInVainOfRow && turnsThatRunOtherCodeComeRound;
        if (otherCodeIsKeptOutOfFirstCopy)
            turnsThatRunOtherCodeComeRound = false;
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.last().turnsThatRunOtherCodeComeRound = turnsThatRunOtherCodeComeRound;
        if (entryOfClosedCopy && lesserBound) {
            m_waysIntoClosedCopies.last().relationThatAsksLesserBound = heldBelowRoom.relation;
            m_waysIntoClosedCopies.last().lesserBound = lesserBound;
            m_waysIntoClosedCopies.last().boundBesideLesserBound = boundBesideLeastRoom;
            m_waysIntoClosedCopies.last().viewWhoseLengthIsBound = viewWhoseLengthIsBound;
            m_waysIntoClosedCopies.last().counterBelowRoom = WTF::move(counterBelowRoom);
        } else if (entryOfClosedCopy && lesserOfBoundAndGreatestInt32) {
            m_waysIntoClosedCopies.last().relationThatAsksLesserBound = relationOfHeadOnNumber.relation;
            m_waysIntoClosedCopies.last().lesserBound = lesserOfBoundAndGreatestInt32;
            m_waysIntoClosedCopies.last().boundBesideLesserBound = boundRoundedInFront;
            m_waysIntoClosedCopies.last().variableThatIsBound = relationOfHeadOnNumber.variable;
            m_waysIntoClosedCopies.last().readOfVariableThatIsBound = readOfVariableThatIsBound;
        }
        for (const CounterTestedInFrontOfLoop& counter : counters) {
            if (entryOfClosedCopy && counter.isHeldWhereItComesRound) {
                m_waysIntoClosedCopies.last().countersHeld.append({ counter.position, counter.tested });
                if (m_types.at(counter.tested).hasObjects())
                    count(StatisticID::LinksHeldToTheirLayoutWhereTheyComeRound);
            }
        }
        ++m_ordinalOfCopyOfLoop;
        if (turnsThatRunOtherCodeComeRound)
            m_effectsSeededForNextRow = preview.effects;
        else if (typedCopyIsQuietByRow || listsRead.areTested)
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
        uint32_t listsAskedInFront = m_numberOfListsAskedInFrontOfTypedCopies;
        uint32_t elementsAskedInFront = m_numberOfElementsAskedInFrontOfTypedCopies;
        uint32_t numbersReadInFront = m_numberOfNumbersReadInFrontOfTypedCopies;
        m_walkOfLoopWasKept = false;
        m_buildsTestsBesidePlainWalk = false;
        {
            SetForScope headOverLists(m_headOfTypedBuildOverListsTestedInFront, listsRead.areTested ? inputHead : BlockID());
            SetForScope frameOverLists(m_frameOfTypedBuildOverListsTestedInFront, frameIndex);
            Vector<uint32_t, 2> listsMadeHere;
            for (const ListTestedInFrontOfLoop& list : listsRead.lists) {
                if (listsRead.areTested && list.name.isValue() && !list.isWritable)
                    listsMadeHere.append(rootOfCellInSweptCode(m_code, list.name.id()).index());
            }
            SetForScope listsOverLists(m_listsMadeHereOfTypedBuildOverListsTestedInFront, WTF::move(listsMadeHere));
            walkCopyOfLoop(inputHead, enteringTypedCopy.span());
        }
        m_typedBuildTestsNothing = !hasTestInFront && variablesCheckedInTurns.isEmpty() && m_numberOfGuessesOpened == guessesOpenedInFront;
        m_typedBuildAskedNoListThatItWasBoughtFor = (turnsAreQuietBehindListsFoundFromInFrontAlone || isBoughtForListsFoundFromInFront) && !hasTestInFront && variablesCheckedInTurns.isEmpty() && !holdsCounterBelowRoom && m_numberOfListsAskedInFrontOfTypedCopies == listsAskedInFront;
        m_walkOfLoopWasKept &= m_headOfLoopKeptLast == inputHead && m_frameOfLoopKeptLast == frameIndex;
        m_typedBuildAsksGuardsThatItWasBoughtFor = guardsNotedAreOnlyTestInFront && !isBoughtForBoundAlone && !isBoughtForListsFoundFromInFront && variablesCheckedInTurns.isEmpty() && !ofTypedCopy && !holdsCounterBelowRoom && m_walkOfLoopWasKept
            && std::ranges::any_of(m_guardsOfStoresAskedOfRow, [&](const MemberConditionRecord& noted) { return m_guardsOfStoresAskedInLoopKeptLast.contains(noted); });
        if (isBoughtForListsFoundFromInFront && m_walkOfLoopWasKept && !m_typedBuildAskedNoListThatItWasBoughtFor)
            count(StatisticID::QuietLoopsBoughtForListsFoundFromInFront);
        bool failingInFrontIsNoRarity = entryOfClosedCopy && m_waysIntoClosedCopies.last().frontAsksWhatNothingSpeaksFor;
        if (failingInFrontIsNoRarity && closedStart)
            m_code.makeBlockUnlikely(closedStart, true);
        if (m_walkOfLoopWasKept)
            m_reportOfTypedBuildMadeLast = ReportOfTypedBuild { m_effectsOfLoopKeptLast, otherCopyOpensItsGuesses, !failingInFrontIsNoRarity && !likelyWayLeadsToClosedStart && (rareWayLeadsToClosedStart || !testsOfListsLeadToClosedStart) };
        if (listsRead.areTested && !m_hasFailed && m_walkOfLoopWasKept && !leavesListsTestedInFrontOfLoopAsTheyAre(*this, m_effectsOfLoopKeptLast, listsRead.lists.span(), inputHead))
            m_typedBuildEndedListsTestedInFront = true;
        else if (listsRead.areTested && !m_hasFailed && m_walkOfLoopWasKept && !leavesListsTestedInFrontOfLoopAsTheyAre(*this, m_effectsOfLoopKeptLast))
            count(StatisticID::LoopsOverTestedListsThatFillWhatIsMadeHere);
        if (!m_hasFailed && m_walkOfLoopWasKept && m_numberOfElementsAskedInFrontOfTypedCopies != elementsAskedInFront && (m_effectsOfLoopKeptLast.writes.maskOf(AbstractHeap::Elements) & markOfElementsOfArraysNotStoredAsRawDoubles))
            m_typedBuildEndedListsTestedInFront = true;
        bool turnsStoreElements = m_effectsOfLoopKeptLast.mayRunOtherCode() || m_effectsOfLoopKeptLast.writes.maskOf(AbstractHeap::Elements);
        if (!m_hasFailed && m_walkOfLoopWasKept && m_numberOfNumbersReadInFrontOfTypedCopies != numbersReadInFront && turnsStoreElements)
            m_typedBuildEndedListsTestedInFront = true;
        m_numberOfNumbersReadInFrontOfTypedCopies = numbersReadInFront;
        m_numberOfElementsAskedInFrontOfTypedCopies = elementsAskedInFront;
        m_effectsSeededForNextRow = std::nullopt;
        if ((isBoughtForBoundAlone || (holdsCounterBelowRoom && !hasTestInFront)) && variablesCheckedInTurns.isEmpty() && !ofTypedCopy && !(entryOfClosedCopy && m_waysIntoClosedCopies.last().headAsksLesserBound))
            m_everyTurnOfTypedBuildLeaves = true;
        m_typedBuildIsLeftAtOtherCodeOnUsualWay = false;
        if (BlockID typedHead = inputBlock(frameIndex, inputHead).head; typedHead && otherCodeMayStay && !turnsThatRunOtherCodeComeRound && m_waysIntoClosedCopies.last().isLeftAtOtherCodeOnUsualWay) {
            std::span<const ValueID> carried = m_code.at(typedHead).parameters();
            std::span<const ValueID> taken = m_code.at(entryOfClosedCopy).parameters();
            m_typedBuildIsLeftAtOtherCodeOnUsualWay = !sharedEntry && !otherCopyOpensItsGuesses;
            for (size_t place = 0; place < std::min(carried.size(), taken.size()); ++place)
                m_typedBuildIsLeftAtOtherCodeOnUsualWay |= m_code.at(taken[place]).representation == Representation::JSValue && m_code.at(carried[place]).representation != Representation::JSValue;
            if (m_waysIntoClosedCopies.last().headAsksLesserBound && m_everyTurnOfTypedBuildLeaves && !m_otherCodeStayedInVainOfRow) {
                m_typedBuildIsLeftAtOtherCodeOnUsualWay = true;
                m_everyTurnOfTypedBuildLeaves = false;
            }
        }
        copyBesideIsEntered = !isCopyBeside && entryOfClosedCopy && m_waysIntoClosedCopies.last().guessOfSomeTurnsIsAskedInFront;
        copyBesideIsBoughtByItself = copyBesideIsEntered && m_waysIntoClosedCopies.last().questionOfEveryTurnIsAskedInFront;
        if (otherCodeIsKeptOutOfFirstCopy && (m_waysIntoClosedCopies.last().isLeftAtOtherCodeOnUsualWay || m_waysIntoClosedCopies.last().isLeftAtAccessOnUsualWay || m_everyTurnOfTypedBuildLeaves)) {
            m_everyTurnOfTypedBuildLeaves = false;
            m_typedBuildIsLeftAtOtherCodeOnUsualWay = true;
            m_otherCodeIsKeptOutOfFirstCopyInVainOfRow = true;
            copyBesideIsEntered = false;
        }
        if (entryOfClosedCopy)
            dropWayIntoClosedCopy();
        for (auto [name, reference] : boundBefore)
            bind(name, reference);
    }
    if (startOfCopyBeside && !copyBesideIsEntered && !m_hasFailed) {
        m_state = WTF::move(stateOfCopyBeside);
        m_block = startOfCopyBeside;
        jumpTo(closedStart, { });
    }
    if (listsRead.whereKindIsAnother && !m_hasFailed) {
        for (BlockID member : index.membersOf(loop))
            inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
        forgetValuesOfLoop(inputHead);
        standInOtherBlock(inputHead);
        m_frames[frameIndex].standsInFrontOfHead = true;
        continueIn(listsRead.whereKindIsAnother);
        if (knownInFrontOfTestsOfLists && !isUnreachable())
            m_state = WTF::move(*knownInFrontOfTestsOfLists);
        if (!isUnreachable() && (!entryOfClosedCopy || !closedStart || !typedStart || (sharedEntry && !sharedCopyIsPlainWalk))) {
            if (!closedStart) {
                fail("a list of another kind of storage finds no closed copy"_s);
                return true;
            }
            jumpTo(closedStart, { });
        } else if (!isUnreachable()) {
            putBackIterationsSteppedAt(steppedAtHead);
            count(StatisticID::LoopsBoughtOnceMoreForListsOfAnotherKindOfStorage);
            SetForScope forAnotherKind(m_walksCopyForAnotherKindOfStorage, true);
            SetForScope walkWasKept(m_walkOfLoopWasKept, m_walkOfLoopWasKept);
            if (sharedCopyIsPlainWalk) {
                SetForScope besidePlainWalk(m_buildsTestsBesidePlainWalk, true);
                SetForScope buys(m_buysTwoBuilds, true);
                bool endedLists = m_typedBuildEndedListsTestedInFront;
                bool asksGuards = m_typedBuildAsksGuardsThatItWasBoughtFor;
                bool everyTurnLeaves = m_everyTurnOfTypedBuildLeaves;
                bool leavesWithoutTest = m_turnOfTypedBuildLeavesWithoutTest;
                bool testsNothing = m_typedBuildTestsNothing;
                bool isLeftAtOtherCode = m_typedBuildIsLeftAtOtherCodeOnUsualWay;
                bool askedNoList = m_typedBuildAskedNoListThatItWasBoughtFor;
                std::optional<ReportOfTypedBuild> ofFirstCopy = std::exchange(m_reportOfTypedBuildMadeLast, std::nullopt);
                size_t sharedInFront = m_closedCopiesShared.size();
                m_closedCopiesShared.append(ClosedCopyShared { frameIndex, inputHead, closedStart, entryOfClosedCopy, true, true });
                ++m_ordinalOfCopyOfLoop;
                bool isBuilt = buildLoopTwice(inputHead, preview, entering);
                m_closedCopiesShared.shrink(sharedInFront);
                if (!isBuilt && !m_hasFailed && !isUnreachable())
                    jumpTo(closedStart, { });
                std::optional<ReportOfTypedBuild> ofSecondCopy = std::exchange(m_reportOfTypedBuildMadeLast, std::nullopt);
                if (ofFirstCopy && (ofSecondCopy || !isBuilt)) {
                    if (ofSecondCopy) {
                        ofFirstCopy->effects.merge(ofSecondCopy->effects);
                        ofFirstCopy->otherCopyOpensItsGuesses &= ofSecondCopy->otherCopyOpensItsGuesses;
                        ofFirstCopy->startOfOtherCopyIsRare &= ofSecondCopy->startOfOtherCopyIsRare;
                    }
                    m_reportOfTypedBuildMadeLast = WTF::move(ofFirstCopy);
                }
                m_typedBuildEndedListsTestedInFront |= endedLists;
                m_typedBuildAsksGuardsThatItWasBoughtFor |= asksGuards;
                m_everyTurnOfTypedBuildLeaves |= everyTurnLeaves;
                m_turnOfTypedBuildLeavesWithoutTest |= leavesWithoutTest;
                m_typedBuildTestsNothing |= testsNothing;
                m_typedBuildIsLeftAtOtherCodeOnUsualWay |= isLeftAtOtherCode;
                m_typedBuildAskedNoListThatItWasBoughtFor |= askedNoList;
            } else {
                SetForScope endedLists(m_typedBuildEndedListsTestedInFront, m_typedBuildEndedListsTestedInFront);
                SetForScope asksGuards(m_typedBuildAsksGuardsThatItWasBoughtFor, m_typedBuildAsksGuardsThatItWasBoughtFor);
                SetForScope everyTurnLeaves(m_everyTurnOfTypedBuildLeaves, m_everyTurnOfTypedBuildLeaves);
                SetForScope leavesWithoutTest(m_turnOfTypedBuildLeavesWithoutTest, m_turnOfTypedBuildLeavesWithoutTest);
                SetForScope testsNothing(m_typedBuildTestsNothing, m_typedBuildTestsNothing);
                SetForScope isLeftAtOtherCode(m_typedBuildIsLeftAtOtherCodeOnUsualWay, m_typedBuildIsLeftAtOtherCodeOnUsualWay);
                SetForScope askedNoList(m_typedBuildAskedNoListThatItWasBoughtFor, m_typedBuildAskedNoListThatItWasBoughtFor);
                SetForScope report(m_reportOfTypedBuildMadeLast, m_reportOfTypedBuildMadeLast);
                m_closedCopiesShared.append(ClosedCopyShared { frameIndex, inputHead, closedStart, entryOfClosedCopy, true });
                m_waysIntoClosedCopies.append(WayIntoClosedCopy { frameIndex, inputHead, entryOfClosedCopy, { }, closedStart, { }, false });
                m_waysIntoClosedCopies.last().turnsCall = someOperationOfRowThatRanOtherCodeIsCall(index), m_waysIntoClosedCopies.last().someCallStaysInTurn = m_someCallStaysInTurnOfRow;
                m_waysIntoClosedCopies.last().reckonsWithWhatItReads = loopReckonsWithWhatItReads(inputHead);
                m_waysIntoClosedCopies.last().lastBlockThatEveryTurnPasses = BlocksThatEveryTurnPasses(index, index.loopOf(inputHead)).lastOf(index.loopOf(inputHead));
                ++m_ordinalOfCopyOfLoop;
                std::pair<uint32_t, BlockID> boughtFirst { static_cast<uint32_t>(frameIndex), inputHead };
                bool firstPurchaseIsListed = !m_loopsBeingBought.isEmpty() && m_loopsBeingBought.last() == boughtFirst;
                if (firstPurchaseIsListed)
                    m_loopsBeingBought.removeLast();
                walkCopyOfLoop(inputHead, entering);
                if (firstPurchaseIsListed)
                    m_loopsBeingBought.append(boughtFirst);
                dropWayIntoClosedCopy();
                m_closedCopiesShared.removeLast();
            }
        }
    }
    takeIterationsAsTheyEntered();

    for (BlockID member : index.membersOf(loop))
        inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
    forgetValuesOfLoop(inputHead);
    if (frontIsEnteredOutOfTurns && closedStart && !m_hasFailed) {
        if (m_block != closedStart) {
            m_block = { };
            m_state.clear();
            continueIn(closedStart);
        }
        if (isUnreachable()) {
            bool hoistedTestsLeadHere = false;
            m_code.forEachEdgeIntoSoFar(closedStart, [&](const Edge&) {
                hoistedTestsLeadHere = true;
            });
            if (hoistedTestsLeadHere) {
                m_block = closedStart;
                m_state = WTF::move(stateOfClosedStartOfItsOwn);
            }
        }
        if (!isUnreachable() && entryTakes(sharedEntry, entering))
            enterWithFieldsOfScopes(sharedEntry, entering);
        else if (!isUnreachable()) {
            count(StatisticID::FrontsEnteredOutOfTurnsWithClosedCopiesOfTheirOwn);
            {
                ClosedArm closedArm(*this);
                SetForScope isBuiltForSize(m_numberOfCopiesBuiltForSize, m_numberOfCopiesBuiltForSize + 1);
                walkCopyOfLoop(inputHead, entering);
            }
            for (BlockID member : index.membersOf(loop))
                inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
            forgetValuesOfLoop(inputHead);
        }
    }
    if (entryOfClosedCopy && !sharedEntry)
        forgetWhatWasBelievedAt(entryOfClosedCopy);
    m_frames[frameIndex].loopsBeingBuiltTwice.removeLast();
    leaveBlockPending();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
