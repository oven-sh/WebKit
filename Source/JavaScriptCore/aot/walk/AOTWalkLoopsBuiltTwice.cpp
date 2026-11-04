/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LoopInvariants with Guesses: the tests of all guesses at values that a loop does not change stand in a row in front of it. Where all pass the loop is walked with the proofs bound all round.
// Where one fails a closed copy of the loop runs, built for size, in which no guess is opened and no loop is built twice. Behind the loop the two meet as any two edges do, and the proofs are gone.
// At a suspension: a loop that may suspend is built once. At a throw: each copy throws to the handlers of its blocks as any block does. A handler gets no name of what the loop has defined: such a loop is built once.
// Withheld, either of the two: every loop is built once.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// In operations of the body. The closed copy costs about as many bytes as the loop has closed operations, and it is run where a written type is wrong.
static constexpr uint32_t largestLoopBuiltTwice = 100;

bool Walker::leavesLoopBeingBuiltTwice(size_t frameIndex, BlockID to) const
{
    const Frame& frame = m_frames[frameIndex];
    return std::ranges::any_of(frame.loopsBeingBuiltTwice, [&](LoopID loop) {
        return !frame.index->isInLoop(to, loop);
    });
}

// What is read behind an edge is defined on every way to it, so something stands for it where the edge leaves.
void Walker::takeNamesOntoEdge(OpenEdge& edge, size_t frameIndex, BlockID to)
{
    const Frame& frame = m_frames[frameIndex];
    if (frame.valuesBuiltTwice.isEmpty())
        return;
    if (!m_liveness.isComputedFor(*frame.index))
        m_liveness.compute(*frame.index);
    for (ValueID value : frame.valuesBuiltTwice) {
        if (m_liveness.isLiveAtHead(value, to))
            edge.names.append({ value, frame.references[value.index()] });
    }
}

// No value is live for the sake of a hint, so no edge has brought the block being walked what stands for it here.
bool Walker::isWithoutNameBehindLoopBuiltTwice(ValueID value)
{
    const Frame& frame = m_frames.last();
    if (!frame.valuesBuiltTwice.contains(value) || frame.index->blockOf(value) == frame.block)
        return false;
    if (!m_liveness.isComputedFor(*frame.index))
        m_liveness.compute(*frame.index);
    return !m_liveness.isLiveAtHead(value, frame.block);
}

// The walk stands in a block without an end, which enters the copy alone.
void Walker::walkCopyOfLoop(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);

    // The names that hold at the head come from in front of the loop.
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
    takeNamesOntoEdge(open, frameIndex, inputHead);
    m_block = { };

    size_t numberOfLoopsAround = m_openLoops.size();
    for (BlockID member : index.membersOf(loop)) {
        if (m_hasFailed)
            break;
        walkBlock(member);
    }
    endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
    m_block = { };
    m_state.clear();
}

ValueID Walker::findReadAskedAtHeadOfLoop(BlockID inputHead, std::span<const std::pair<Reference, TypeID>> tested)
{
    const BodyIndex& index = currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    // A text in its builder and the position of an iteration by index have no value for the head of the closed copy.
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

    // What the head holds at this turn is what the closed copy would hold at it: both are the same program, and nothing of the turn has run.
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
    noteGuessOpened();
    return true;
}

void Walker::noteHeadOfTurn(BlockID inputHead)
{
    // A turn of a loop inside is no part of the turn that is run once.
    m_state.noteSomethingWasDoneInTurn();
    if (m_waysIntoClosedCopies.isEmpty() || m_numberOfCopiesBuiltForSize || isUnreachable())
        return;
    WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (way.frameIndex != m_frames.size() - 1 || way.inputHead != inputHead)
        return;
    BodyBlock head = currentBodyIndex().block(inputHead);
    std::span<const ValueID> taken = m_code.at(way.entry).parameters();
    way.heldAtHead.shrink(0);
    if (taken.size() != head.numberOfParameters() || m_code.at(m_block).parameters().size() != taken.size())
        return;
    for (uint32_t index = 0; index < head.numberOfParameters(); ++index) {
        Reference reference = referenceOf(head.parameter(index));
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
        || m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) || isLoopThatMayDoAnything(currentFunction(), inputHead))
        return false;
    // A loop inside is looked at as it stands, and seldom runs nothing.
    return !preview.otherCodeRunsInLoopInside && currentBodyIndex().loop(currentBodyIndex().loopOf(inputHead)).numberOfLoopsWithin <= 1;
}

std::unique_ptr<LoopPreview> Walker::previewTypedCopy(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, std::span<const ListTestedInFrontOfLoop> lists)
{
    if (!turnsOfTypedCopyMayBeginAgain(inputHead, preview))
        return nullptr;
    // `items[i].price`: behind the row in front of the loop the read out of the list runs no code, and the look that has found the lists has come through the whole turn.
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
    // `text = text + "b"`: beginTextsInStringBuilders() lets the text go round in a builder, which the head of the other copy does not take: with a text in a builder no turn begins again.
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
    // The other copy of the loop itself is walked before there is a way into it, and at the head of a loop inside something counts as done.
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
    // What the state knows of it beyond its own type may have been learned in the loop.
    return loop && reference && reference.isRecord() && standsInFrontOfLoop(*loop, reference) && typeOf(reference) == ownTypeOf(reference);
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
    m_walker.m_block = m_blockInside;
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
    // The call stands in the loop that is looked at.
    frame.loopDepthAtCall = m_frames.last().loopDepthAtCall + 1;
    frame.inlineFrame = m_frames.last().inlineFrame;
    frame.block = entryBlock;
    m_frames.append(WTF::move(frame));
    // As walkInline() counts, so that the look is told of a callee of the copy what the walk will be told.
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

bool Walker::buildLoopTwice(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Guesses) || !m_opensGuesses || !index.isLoopHead(inputHead) || index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead))
        return false;
    // An edge brings such a parameter what nothing reads, so no edge behind the loop would have a name for it.
    if (index.holdsParametersReadByReleaseHintsAlone())
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop))
        return false;
    // The position of an iteration by index that was opened in front of the loop comes round in a parameter of the one head.
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) && !iterationsSteppedAtMayBeBuiltTwice(inputHead))
        return false;

    uint32_t numberOfOperations = 0;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        // The block of a return throws to a handler of the loop. In the copy of a callee it stays open for walkInline(), which takes it up behind a walkBlock() of its own.
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

    Vector<GuessInFrontOfLoop, 4> guesses = findGuessesInFrontOfLoop(*this, inputHead, preview);
    // The looks that are tried for the lists count on what the look at the typed copy will count on.
    BlockID headOfLooksTriedForLists = turnsOfTypedCopyMayBeginAgain(inputHead, preview) ? inputHead : m_inputHeadAskedAboutTypedCopy;
    ListsReadInLoop listsRead;
    {
        SetForScope asked(m_inputHeadAskedAboutTypedCopy, headOfLooksTriedForLists);
        listsRead = findListsReadInLoop(*this, inputHead, preview, entering, guesses);
    }
    Vector<CounterTestedInFrontOfLoop, 2> counters = findCountersTestedInFrontOfLoop(*this, inputHead, entering, guesses);
    Vector<std::pair<Reference, TypeID>, 4> testedInFront;
    for (const GuessInFrontOfLoop& guess : guesses)
        testedInFront.append({ guess.value, guess.tested });
    bool asksAtHead = !!findReadAskedAtHeadOfLoop(inputHead, testedInFront.span());
    bool hasTestInFront = !guesses.isEmpty() || !listsRead.lists.isEmpty() || !counters.isEmpty() || asksAtHead;
    bool otherCopyOpensItsGuesses = guesses.isEmpty() && counters.isEmpty() && !asksAtHead;
    std::unique_ptr<LoopPreview> ofTypedCopy = previewTypedCopy(inputHead, preview, entering, listsRead.areLeftAsTheyAre ? listsRead.lists.span() : std::span<const ListTestedInFrontOfLoop>());
    if (!hasTestInFront && !ofTypedCopy)
        return false;
    // `total += area(box)`: what a turn expects of a parameter of the head is asked of what enters, once. What comes round is typed by the turns.
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
    // A SECOND COPY PAYS WHERE SOMETHING LEAVES THE TURNS: a test that answers the same in every turn, with the reads behind it, or the box of what goes round. `while (node) node = node.parent` asks each turn anew in either copy.
    if (!hasTestInFront && counters.isEmpty()) {
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
            // `while (pos < end && text.charCodeAt(pos) === 32) pos++`, all three variables of a module or shared by closures: the turns write `pos` alone.
            if (definition.opcode() == Opcode::GetModuleVariable)
                return !declaredEffectsOfOperation(program(), Opcode::GetModuleVariable, definition).reads.overlaps(ofTurns.writes);
            if (definition.opcode() == Opcode::GetEnvironmentField)
                return !declaredEffectsOfOperation(program(), Opcode::GetEnvironmentField, definition).reads.overlaps(ofTurns.writes) && self(self, definition.environmentOperand(), depth - 1);
            if (definition.opcode() != Opcode::PropGet || definition.isFreeName() || declaredBoundOf(Opcode::GetSlot).reads.overlaps(ofTurns.writes))
                return false;
            return self(self, definition.objectOperand(), depth - 1);
        };
        bool somethingLeavesTheTurns = ofTypedCopy->guessesCountedOn.containsIf([&](const GuessCountedOnByLooks& guess) {
            return isTheSameInEveryTurn(isTheSameInEveryTurn, guess.operand, 4);
        });
        if (!somethingLeavesTheTurns)
            return false;
    }

    // What the loop defines and code behind it reads.
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
            return false;
    }

    Origin origin = m_origin;
    BlockID closedStart;
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
    // Whatever stands between it and the head of the closed copy is built behind it, so it holds on every way in.
    BlockID entryOfClosedCopy;
    Vector<Reference, 8> enteringClosedCopy;
    // A text in its builder and the position of an iteration by index have no value for the head of the closed copy.
    if (asksAtHead || (!hasTextsInStringBuilders() && !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)))) {
        entryOfClosedCopy = addBlock(otherCopyOpensItsGuesses ? BlockFrequency::Normal : BlockFrequency::Rare);
        for (size_t index = 0; index < entering.size(); ++index) {
            TypeID type = joinTypes(preview.typesOfParameters[index], ownTypeOf(entering[index]));
            enteringClosedCopy.append(addParameter(entryOfClosedCopy, type, formOfParameter(type, { })));
        }
        settleParametersOf(entryOfClosedCopy);
    }

    // A loop inside a copy may have listed some of them.
    for (ValueID value : readBehindLoop) {
        if (!m_frames[frameIndex].valuesBuiltTwice.contains(value))
            m_frames[frameIndex].valuesBuiltTwice.append(value);
    }
    m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
    uint32_t namesInFrontOfLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;
    // The second copy begins as the first: nothing stands for a value of the loop yet. A link that looks ahead in the text would find what the other copy has bound.
    auto forgetValuesOfLoop = [&] {
        for (BlockID member : index.membersOf(loop)) {
            BodyBlock block = index.block(member);
            for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
                m_frames[frameIndex].references[value] = Reference();
        }
    };

    // walkBlock() forgets the blocks that wait, with their states.
    BlockID typedStart = isUnreachable() ? BlockID() : m_block;
    WalkState stateWhereTestsPassed = m_state;
    IterationsSteppedAtHead steppedAtHead = takeIterationsSteppedAt(inputHead);
    if (closedStart)
        continueIn(closedStart);
    else {
        // Whatever test is asked in front of the typed copy later on fails to here.
        if (entryOfClosedCopy)
            closedStart = addBlock(BlockFrequency::Rare);
        m_block = closedStart;
        m_state.clear();
    }
    if (entryOfClosedCopy) {
        if (!isUnreachable())
            endWithJumpInto(entryOfClosedCopy, entering);
        // A turn of the typed copy may have ended what held in front of the loop.
        m_block = entryOfClosedCopy;
        m_state.clear();
    }
    // Nothing was asked in front of the loop but the kinds of the lists that it reads. What is written of the elements of a list is no more than a guess at its kind, and one text among the numbers fails it for every turn.
    // So the other copy is the loop as it is built without the row, with the guesses of its own turns.
    // Or nothing at all, or what enters for a parameter of the head: whatever fails in the typed copy meets what the walk has always built of the loop.
    if (otherCopyOpensItsGuesses)
        walkCopyOfLoop(inputHead, entryOfClosedCopy ? enteringClosedCopy.span() : entering);
    else {
        ClosedArm closedArm(*this);
        ++m_numberOfCopiesBuiltForSize;
        walkCopyOfLoop(inputHead, entryOfClosedCopy ? enteringClosedCopy.span() : entering);
        --m_numberOfCopiesBuiltForSize;
    }
    forgetValuesOfLoop();

    if (typedStart && !m_hasFailed) {
        m_block = typedStart;
        m_state = WTF::move(stateWhereTestsPassed);
        putBackIterationsSteppedAt(steppedAtHead);
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
        // Nothing was tested in front of the loop: the typed copy is entered under the types that the look began with.
        if (entryOfClosedCopy && !hasTestInFront && counters.isEmpty())
            m_previewOfTypedCopyAboutToBeEntered = WTF::move(ofTypedCopy);
        walkCopyOfLoop(inputHead, enteringTypedCopy.span());
        m_previewOfTypedCopyAboutToBeEntered = nullptr;
        if (entryOfClosedCopy)
            m_waysIntoClosedCopies.removeLast();
        for (auto [name, reference] : boundBefore)
            bind(name, reference);
    }

    // Behind the loop nothing of either copy is at hand but what the edges bring: neither what stands for a value of the loop, nor a narrower name that holds in a block of a copy.
    for (BlockID member : index.membersOf(loop))
        inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
    forgetValuesOfLoop();
    m_frames[frameIndex].loopsBeingBuiltTwice.removeLast();
    m_block = { };
    m_state.clear();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
