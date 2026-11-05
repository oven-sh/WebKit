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
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t largestLoopBuiltTwice = 100;

bool Walker::leavesLoopBeingBuiltTwice(size_t frameIndex, BlockID to) const
{
    const Frame& frame = m_frames[frameIndex];
    return std::ranges::any_of(frame.loopsBeingBuiltTwice, [&](LoopID loop) {
        return !frame.index->isInLoop(to, loop);
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
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);

    uint32_t namesAtHead = inputBlock(frameIndex, inputHead).firstNarrowerName;
    for (BlockID member : index.membersOf(loop))
        inputBlock(frameIndex, member) = InputBlock { };
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
        walkBlock(member);
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
    if (taken.size() != head.numberOfParameters())
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
    uint32_t numberOfValuesInFrontOfTest = m_code.numberOfValues();
    Reference test = buildTestOfObjectAsMade(*this, object, originOf(read));
    if (!test || m_hasFailed)
        return false;
    BlockID passed = addBlock();
    BlockID failed = addBlock(BlockFrequency::Rare);
    branch(test, passed, failed);
    continueIn(failed);
    if (!isUnreachable())
        endWithJumpInto(way.entry, held.span());
    continueIn(passed);
    if (m_hasFailed || isUnreachable())
        return false;
    noteListAsMade(*this, object);
    noteObjectPassedTestOfBeingAsMade(*this, object, test, numberOfValuesInFrontOfTest);
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
    if (taken.size() < head.numberOfParameters() || heldByHead.size() != taken.size())
        return;
    for (uint32_t index = 0; index < taken.size(); ++index) {
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

void Walker::proveWhatLooksCountOnAt(const BodyValue& operation)
{
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (way.guessesCountedOn.isEmpty() || way.frameIndex != m_frames.size() - 1 || !m_opensGuesses)
        return;
    for (size_t place = 0; place < m_waysIntoClosedCopies.last().guessesCountedOn.size(); ++place) {
        GuessCountedOnByLooks guess = m_waysIntoClosedCopies.last().guessesCountedOn[place];
        if (guess.operation == operation.id() && mayBeginTurnAgainInClosedCopy())
            proveExpectedTypeOrBeginTurnAgain(*this, guess.operand, guess.expected, draftOf(operation).origin());
    }
}

bool Walker::turnsOfTypedCopyMayBeginAgain(BlockID inputHead, const LoopPreview& preview) const
{
    size_t frameIndex = m_frames.size() - 1;
    if (!preview.effects.mayRunOtherCode() || hasTextsInStringBuilders() || m_numberOfCopiesBuiltForSize || isUnderHandler()
        || (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !iterationsSteppedAtHandOverPositionsAlone(inputHead)) || isLoopThatMayDoAnything(currentFunction(), inputHead))
        return false;
    return !preview.otherCodeRunsInLoopInside && currentBodyIndex().loop(currentBodyIndex().loopOf(inputHead)).numberOfLoopsWithin <= 1;
}

std::unique_ptr<LoopPreview> Walker::previewTypedCopy(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, std::span<const ListTestedInFrontOfLoop> lists, std::unique_ptr<LoopPreview> madeWithListsNoted)
{
    if (!turnsOfTypedCopyMayBeginAgain(inputHead, preview))
        return nullptr;
    if (madeWithListsNoted) {
        if (madeWithListsNoted->effects.mayRunOtherCode())
            return nullptr;
        for (size_t parameter = 0; parameter < madeWithListsNoted->typesOfParameters.size(); ++parameter) {
            if (!m_types.isSubset(madeWithListsNoted->typesOfParameters[parameter], preview.typesOfParameters[parameter]))
                return nullptr;
        }
        if (mayKeepTextsInStringBuilders(*this) && !madeWithListsNoted->effects.maySuspend) {
            Vector<GrowingText, 2> texts;
            findGrowingTexts(*this, inputHead, *madeWithListsNoted, texts);
            if (!texts.isEmpty())
                return nullptr;
        }
        return madeWithListsNoted;
    }
    ListsNotedAsTestedForLook noted(*this, lists);
    if (ValueID first = lists.empty() ? preview.firstOperationThatRunsOtherCode : ValueID()) {
        auto typeInLooks = [&](ValueID input) -> TypeID {
            if (input.index() >= preview.firstValue && input.index() - preview.firstValue < preview.typesOfValues.size())
                return preview.typesOfValues[input.index() - preview.firstValue];
            Reference built = referenceOf(input);
            return built ? typeOf(built) : TypeID();
        };
        if (!guessesMayTypeOperationInCleanTurn(*this, currentBodyIndex().value(first), TypesOfInputs(typeInLooks)))
            return nullptr;
    }
    Vector<TypeID, 8> entryTypes;
    for (Reference reference : entering)
        entryTypes.append(ownTypeOf(reference));
    SetForScope asked(m_inputHeadAskedAboutTypedCopy, inputHead);
    LoopPreview ofTypedCopy = previewLoop(*this, inputHead, entryTypes.span(), preview.typesOfParameters.span());
    if (m_hasFailed || ofTypedCopy.effects.mayRunOtherCode())
        return nullptr;
    if (mayKeepTextsInStringBuilders(*this) && !ofTypedCopy.effects.maySuspend) {
        Vector<GrowingText, 2> texts;
        findGrowingTexts(*this, inputHead, ofTypedCopy, texts);
        if (!texts.isEmpty())
            return nullptr;
    }
    return makeUniqueWithoutFastMallocCheck<LoopPreview>(WTF::move(ofTypedCopy));
}

bool Walker::mayBeginTurnAgainInClosedCopy() const
{
    return !m_waysIntoClosedCopies.isEmpty() && m_waysIntoClosedCopies.last().isEntered && !isUnreachable() && m_state.nothingWasDoneInTurn(m_waysIntoClosedCopies.last().numberOfTurn) && !isUnderHandler() && !hasTextsInStringBuilders();
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
    OpenLoop& loop = m_openLoops[*place];
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
    m_placesOfLoopsBehindBlocks[next.index()] = *place + 1;
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

bool Walker::layFrameForLookIntoCopy(FunctionID callee, const BodyIndex& body)
{
    if (m_frames.size() == m_frames.capacity())
        return false;
    Frame frame;
    frame.function = callee;
    frame.body = &body.body();
    frame.index = &body;
    frame.references.fill(Reference(), body.numberOfValues());
    frame.loopDepthAtCall = m_frames.last().loopDepthAtCall + 1;
    frame.inlineFrame = m_frames.last().inlineFrame;
    frame.block = entryBlock;
    m_frames.append(WTF::move(frame));
    if (!m_numberOfFramesLaidForLooks++)
        m_operationsCopiedAtWrittenCallInWalk = std::exchange(m_operationsCopiedAtWrittenCall, 0);
    m_operationsCopiedAtWrittenCall += facts().summaries().at(callee).numberOfOperations;
    return true;
}

void Walker::takeAwayFrameForLookIntoCopy()
{
    m_frames.removeLast();
    if (!--m_numberOfFramesLaidForLooks)
        m_operationsCopiedAtWrittenCall = m_operationsCopiedAtWrittenCallInWalk;
}

bool Walker::standsInTurnThatLooksCountOn() const
{
    if (m_looksTakeFormsBehindLooksAsBuilt)
        return true;
    return !m_numberOfFramesLaidForLooks && mayBeginTurnAgainInClosedCopy() && m_waysIntoClosedCopies.last().looksCountOnTurnsThatBeginAgain;
}

void Walker::beginTurnAgainInClosedCopy()
{
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    endWithJumpInto(way.entry, way.heldAtHead.span());
    m_block = { };
    m_state.clear();
    ++m_numberOfTurnsThatBeginAgain;
}

bool Walker::mayBuildLoopSeveralTimes(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Guesses) || !m_opensGuesses || !index.isLoopHead(inputHead) || index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead))
        return false;
    if (index.holdsParametersReadByReleaseHintsAlone())
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !iterationsSteppedAtMayBeBuiltTwice(inputHead))
        return false;

    uint32_t numberOfOperations = 0;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        TerminatorKind end = block.terminator().kind();
        if (frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit && (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall))
            return false;
        numberOfOperations += block.numberOfValues() + 1;
        if (numberOfOperations > largestLoopBuiltTwice)
            return false;
        for (BodyValue operation : block.values()) {
            if (maySuspendAsWritten(operation))
                return false;
        }
    }
    return true;
}

std::optional<Vector<ValueID, 8>> Walker::findValuesOfLoopReadBehindIt(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    auto isDefinedInLoop = [&](ValueID value) {
        return value.index() >= firstValue && index.isInLoop(index.blockOf(value), loop);
    };
    Vector<ValueID, 8> readBehindLoop;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (index.isInLoop(target.block(), loop))
                continue;
            m_liveness.forEachLiveAtHead(target.block(), ValueID(firstValue), ValueID(index.numberOfValues()), [&](ValueID live) {
                if (isDefinedInLoop(live) && !readBehindLoop.contains(live))
                    readBehindLoop.append(live);
            });
        }
    }
    std::optional<Vector<BlockID, 4>>& handlerBlocks = m_frames[frameIndex].handlerBlocks;
    if (!handlerBlocks) {
        handlerBlocks.emplace();
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            if (index.block(BlockID(number)).isHandlerBlock())
                handlerBlocks->append(BlockID(number));
        }
    }
    for (BlockID handlerBlock : *handlerBlocks) {
        if (index.isInLoop(handlerBlock, loop))
            continue;
        bool readsWhatLoopDefines = false;
        m_liveness.forEachLiveAtHead(handlerBlock, ValueID(firstValue), ValueID(index.numberOfValues()), [&](ValueID live) {
            readsWhatLoopDefines |= isDefinedInLoop(live);
        });
        if (readsWhatLoopDefines)
            return std::nullopt;
    }
    return readBehindLoop;
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
    if (m_numberOfCopiesBuiltForSize || !m_closedCopiesShared.isEmpty() || isUnreachable() || hasTextsInStringBuilders())
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
    m_state.clear();
    BlockID entryOfClosedCopy = addBlock(BlockFrequency::Rare);
    Vector<Reference, 8> enteringClosedCopy;
    for (size_t place = 0; place < entering.size(); ++place) {
        TypeID type = joinTypes(preview.typesOfParameters[place], ownTypeOf(entering[place]));
        enteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
    }
    settleParametersOf(entryOfClosedCopy);

    for (ValueID value : *readBehindLoop)
        m_frames[frameIndex].listAsBuiltTwice(value);
    m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
    uint32_t namesInFrontOfLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;

    endWithJumpInto(entryOfClosedCopy, entering);
    m_block = entryOfClosedCopy;
    m_state.clear();
    {
        ClosedArm closedArm(*this);
        ++m_numberOfCopiesBuiltForSize;
        walkCopyOfLoop(inputHead, enteringClosedCopy.span());
        --m_numberOfCopiesBuiltForSize;
    }
    forgetValuesOfLoop(inputHead);

    m_closedCopiesShared.append(ClosedCopyShared { frameIndex, inputHead, closedStart, entryOfClosedCopy, false });
    Vector<TypeID> settledAroundCopies = m_frames[frameIndex].typesOfLooks;
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
        walkCopyOfLoop(inputHead, entering);
        m_waysIntoClosedCopies.removeLast();
        m_closedCopiesShared.last().isAboutToBeTaken = false;
        for (auto [name, reference] : boundBefore)
            bind(name, reference);
        forgetValuesOfLoop(inputHead);
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
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    ClosedCopyShared* shared = !m_closedCopiesShared.isEmpty() && m_closedCopiesShared.last().frameIndex == frameIndex && m_closedCopiesShared.last().inputHead == inputHead && std::exchange(m_closedCopiesShared.last().isAboutToBeTaken, false) ? &m_closedCopiesShared.last() : nullptr;
    if (!shared && m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop))
        return false;
    BlockID sharedStart = shared ? shared->start : BlockID();
    BlockID sharedEntry = shared ? shared->entry : BlockID();

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
    bool listsAloneAreAskedInFront = listsRead.look && headOfLooksTriedForLists == inputHead && counters.isEmpty() && !asksAtHead && variablesCheckedInTurns.isEmpty() && guardsAskedOfObjectsOfTurns.isEmpty()
        && std::ranges::all_of(guesses, [&](const GuessInFrontOfLoop& guess) { return listsRead.lists.containsIf([&](const ListTestedInFrontOfLoop& list) { return list.name == guess.value; }); });
    std::unique_ptr<LoopPreview> ofTypedCopy = previewTypedCopy(inputHead, preview, entering, listsRead.lists.span(), headOfLooksTriedForLists == inputHead ? WTF::move(listsRead.look) : nullptr);
    if (!hasTestInFront && variablesCheckedInTurns.isEmpty() && !ofTypedCopy)
        return false;
    otherCopyOpensItsGuesses &= variablesCheckedInTurns.isEmpty() || ofTypedCopy || !listsRead.lists.isEmpty();
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
    if (!sharedEntry && !hasTestInFront && variablesCheckedInTurns.isEmpty() && counters.isEmpty()) {
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
        bool somethingLeavesTheTurns = ofTypedCopy->guessesCountedOn.containsIf([&](const GuessCountedOnByLooks& guess) {
            return isTheSameInEveryTurn(isTheSameInEveryTurn, guess.operand, 4);
        });
        if (!somethingLeavesTheTurns)
            return false;
    }

    std::optional<Vector<ValueID, 8>> foundBehindLoop = findValuesOfLoopReadBehindIt(inputHead);
    if (!foundBehindLoop)
        return false;
    Vector<ValueID, 8> readBehindLoop = WTF::move(*foundBehindLoop);

    Origin origin = m_origin;
    BlockID closedStart = sharedStart;
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
    if (closedStart == sharedStart && !hasTestInFront && !ofTypedCopy)
        return false;
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
        continueIn(passed);
        enteringTypedCopy[counter.position] = buildProofOfNumber(*this, entering[counter.position], test, origin);
    }
    {
        SetForScope asked(m_inputHeadAskedAboutTypedCopy, headOfLooksTriedForLists);
        buildTestsOfListsReadInLoop(*this, inputHead, entering, listsRead, guesses.span(), proofs.span(), closedStart, origin);
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
    if (!sharedEntry && (asksAtHead || (!hasTextsInStringBuilders() && (steppedAtHead.asTheyEntered.isEmpty() ? !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) : iterationsSteppedAtHandOverPositionsAlone(inputHead))))) {
        entryOfClosedCopy = addBlock(otherCopyOpensItsGuesses ? BlockFrequency::Normal : BlockFrequency::Rare);
        for (size_t index = 0; index < entering.size(); ++index) {
            TypeID type = joinTypes(preview.typesOfParameters[index], ownTypeOf(entering[index]));
            enteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
        }
        for (size_t place = 0; place < steppedAtHead.asTheyEntered.size(); ++place) {
            TypeID type = m_types.integer(Range { 0, positionBehindEveryEnd });
            positionsEnteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
        }
        settleParametersOf(entryOfClosedCopy);
    }
    takeIterationsAsTheyEntered();

    for (ValueID value : readBehindLoop)
        m_frames[frameIndex].listAsBuiltTwice(value);
    m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
    uint32_t namesInFrontOfLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;

    BlockID typedStart = isUnreachable() ? BlockID() : m_block;
    WalkState stateWhereTestsPassed = m_state;
    if (!sharedEntry) {
        if (closedStart)
            continueIn(closedStart);
        else {
            if (entryOfClosedCopy)
                closedStart = addBlock(BlockFrequency::Rare);
            m_block = closedStart;
            m_state.clear();
        }
        if (entryOfClosedCopy) {
            if (!isUnreachable()) {
                Vector<Reference, 8> withPositions;
                withPositions.append(entering);
                for (size_t place = 0; place < positionsEnteringClosedCopy.size(); ++place)
                    withPositions.append(steppedAtHead.asTheyEntered[place].second.position);
                endWithJumpInto(entryOfClosedCopy, withPositions.span());
            }
            for (size_t place = 0; place < positionsEnteringClosedCopy.size(); ++place)
                m_iterationsByIndex[steppedAtHead.asTheyEntered[place].first].position = positionsEnteringClosedCopy[place];
            m_block = entryOfClosedCopy;
            m_state.clear();
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
        if (entryOfClosedCopy && counters.isEmpty() && (!hasTestInFront || (listsAloneAreAskedInFront && listsRead.areTested)))
            m_previewOfTypedCopyAboutToBeEntered = WTF::move(ofTypedCopy);
        walkCopyOfLoop(inputHead, enteringTypedCopy.span());
        m_previewOfTypedCopyAboutToBeEntered = nullptr;
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
