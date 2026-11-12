/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool relationHoldsPositionBelowLength(Opcode relation, bool holds, uint32_t placeOfPosition, int64_t distanceBelowLength)
{
    bool positionIsLeft = !placeOfPosition;
    bool isBelowBound = false;
    bool isAtMostBound = false;
    switch (relation) {
    case Opcode::Less:
        isBelowBound = positionIsLeft && holds;
        isAtMostBound = !positionIsLeft && !holds;
        break;
    case Opcode::LessEq:
        isAtMostBound = positionIsLeft && holds;
        isBelowBound = !positionIsLeft && !holds;
        break;
    case Opcode::Greater:
        isBelowBound = !positionIsLeft && holds;
        isAtMostBound = positionIsLeft && !holds;
        break;
    case Opcode::GreaterEq:
        isAtMostBound = !positionIsLeft && holds;
        isBelowBound = positionIsLeft && !holds;
        break;
    default:
        return false;
    }
    return isBelowBound || (isAtMostBound && distanceBelowLength >= 1);
}

namespace {

struct LengthOfTextAsWritten {
    ValueID text;
    int64_t distanceBelowLength { 0 };
};

struct TestOfPositionInText {
    ValueID position;
    ValueID text;
    uint32_t firstDominatorNumber { 0 };
    uint32_t lastDominatorNumber { 0 };

    std::tuple<uint32_t, uint32_t, uint32_t> order() const { return { position.index(), text.index(), firstDominatorNumber }; }
};

enum class BoundOfPositionInText : uint8_t { BelowLength, AtMostLength };

struct TestsOfPositionsInTexts final : Walker::KeptForBuild {
    UncheckedKeyHashMap<const BodyIndex*, Vector<TestOfPositionInText>> belowLengths;
    UncheckedKeyHashMap<const BodyIndex*, Vector<TestOfPositionInText>> atMostLengths;
};

} // anonymous namespace

static ValueID textAsAskedAboutInPositionsInTexts(const BodyIndex& index, ValueID text)
{
    return index.stripCasts(text);
}

static std::optional<LengthOfTextAsWritten> findLengthOfTextAsWritten(const Program& program, const BodyIndex& index, ValueID bound)
{
    LengthOfTextAsWritten found;
    bound = index.stripCasts(bound);
    if (bound && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Sub) {
        BodyValue difference = index.value(bound);
        ValueID less = index.stripCasts(difference.operand(1));
        if (!less || index.isParameter(less) || index.opcodeOf(less) != Opcode::Integer)
            return std::nullopt;
        found.distanceBelowLength = index.value(less).as<Opcode::Integer>().integer();
        if (found.distanceBelowLength < 0)
            return std::nullopt;
        bound = index.stripCasts(difference.operand(0));
    } else if (bound && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Decrement) {
        found.distanceBelowLength = 1;
        bound = index.stripCasts(index.value(bound).operand(0));
    }
    if (!bound || index.isParameter(bound) || index.opcodeOf(bound) != Opcode::PropGet)
        return std::nullopt;
    BodyValue read = index.value(bound);
    if (read.isFreeName() || read.key() != program.keyOf(BuiltinNames::length))
        return std::nullopt;
    found.text = textAsAskedAboutInPositionsInTexts(index, read.objectOperand());
    if (!found.text)
        return std::nullopt;
    return found;
}

static Vector<TestOfPositionInText> collectTestsOfPositionsInTexts(const Program& program, const BodyIndex& index, BoundOfPositionInText bound)
{
    int64_t distanceGranted = bound == BoundOfPositionInText::AtMostLength ? 1 : 0;
    Vector<TestOfPositionInText> tests;
    for (BlockID entered : index.blocksInDominatorOrder()) {
        std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(entered);
        if (!edge)
            continue;
        BodyTerminator terminator = index.terminator(edge->from);
        if (terminator.kind() != TerminatorKind::Branch)
            continue;

        ValueID test = terminator.operand(0);
        bool holds = !edge->targetIndex;
        while (test && !index.isParameter(test)) {
            Opcode opcode = index.opcodeOf(test);
            if (opcode == Opcode::LogicalNot)
                holds = !holds;
            else if (opcode != Opcode::ToBoolean)
                break;
            test = index.value(test).operand(0);
        }
        if (!test || index.isParameter(test) || !allRelations.contains(index.opcodeOf(test)))
            continue;

        BodyValue relation = index.value(test);
        for (uint32_t placeOfPosition = 0; placeOfPosition < 2; ++placeOfPosition) {
            std::optional<LengthOfTextAsWritten> length = findLengthOfTextAsWritten(program, index, relation.operand(1 - placeOfPosition));
            if (!length || !relationHoldsPositionBelowLength(relation.opcode(), holds, placeOfPosition, length->distanceBelowLength + distanceGranted))
                continue;
            ValueID position = index.stripCasts(relation.operand(placeOfPosition));
            if (position)
                tests.append(TestOfPositionInText { position, length->text, index.dominatorNumber(entered), index.lastDominatorNumberBelow(entered) });
        }
    }

    std::ranges::sort(tests, { }, &TestOfPositionInText::order);
    size_t kept = 0;
    for (const TestOfPositionInText& test : tests) {
        bool liesWithinLastKept = kept && tests[kept - 1].position == test.position && tests[kept - 1].text == test.text && test.firstDominatorNumber <= tests[kept - 1].lastDominatorNumber;
        if (!liesWithinLastKept)
            tests[kept++] = test;
    }
    tests.shrink(kept);
    tests.shrinkToFit();
    return tests;
}

static bool testOfBodyHoldsPositionInText(Walker& walker, const BodyIndex& index, BlockID block, ValueID text, ValueID position, BoundOfPositionInText bound)
{
    text = textAsAskedAboutInPositionsInTexts(index, text);
    position = index.stripCasts(position);
    if (!text || !position || !index.isReachable(block))
        return false;

    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByPositionsInTexts();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<TestsOfPositionsInTexts>();
    TestsOfPositionsInTexts& testsKept = static_cast<TestsOfPositionsInTexts&>(*kept);
    auto& ofBodies = bound == BoundOfPositionInText::AtMostLength ? testsKept.atMostLengths : testsKept.belowLengths;
    const Vector<TestOfPositionInText>& tests = ofBodies.ensure(&index, [&] {
        return collectTestsOfPositionsInTexts(walker.program(), index, bound);
    }).iterator->value;

    uint32_t number = index.dominatorNumber(block);
    auto behind = std::ranges::upper_bound(tests, std::tuple(position.index(), text.index(), number), { }, &TestOfPositionInText::order);
    if (behind == tests.begin())
        return false;
    const TestOfPositionInText& last = *std::prev(behind);
    return last.position == position && last.text == text && number <= last.lastDominatorNumber;
}

bool testOfBodyHoldsPositionBelowLengthOfText(Walker& walker, const BodyIndex& index, BlockID block, ValueID text, ValueID position)
{
    return testOfBodyHoldsPositionInText(walker, index, block, text, position, BoundOfPositionInText::BelowLength);
}

namespace {

enum class StepsTaken : uint8_t { Down, Both };

struct SteppedValue {
    ValueID value;
    int64_t offset { 0 };
};

} // anonymous namespace

static SteppedValue stripSteps(const BodyIndex& index, ValueID value, StepsTaken taken)
{
    static constexpr uint32_t maxSteps = 8;
    SteppedValue found { index.stripCasts(value), 0 };
    for (uint32_t depth = 0; depth < maxSteps && found.value && !index.isParameter(found.value); ++depth) {
        BodyValue step = index.value(found.value);
        int64_t by = 0;
        switch (step.opcode()) {
        case Opcode::ToNumeric:
            break;
        case Opcode::Decrement:
            by = -1;
            break;
        case Opcode::Increment:
            by = 1;
            break;
        case Opcode::Add:
        case Opcode::Sub: {
            ValueID written = index.stripCasts(step.operand(1));
            if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::Integer)
                return found;
            int64_t constant = index.value(written).as<Opcode::Integer>().integer();
            if (constant != static_cast<int32_t>(constant))
                return found;
            by = step.opcode() == Opcode::Add ? constant : -constant;
            break;
        }
        default:
            return found;
        }
        if (by > 0 && taken == StepsTaken::Down)
            return found;
        found = { index.stripCasts(step.operand(0)), found.offset + by };
    }
    return found;
}

static bool bodyTestHoldsNumberAfterPosition(Walker& walker, const BodyIndex& index, BlockID block, ValueID text, ValueID position)
{
    static constexpr OpcodeSet stepsDown { Opcode::Decrement, Opcode::Sub, Opcode::Add };
    if (!index.holdsAnyOf(stepsDown))
        return false;
    SteppedValue number = stripSteps(index, position, StepsTaken::Down);
    if (!number.value || number.offset >= 0)
        return false;
    Reference built = walker.referenceOf(number.value);
    if (!built || !built.isRecord() || !walker.types().at(walker.typeOf(built)).isNumber())
        return false;
    return testOfBodyHoldsPositionInText(walker, index, block, text, number.value, BoundOfPositionInText::AtMostLength);
}

static bool isLowered(Walker& walker, const BodyIndex& index, LoopID loop, ValueID counter, ValueID value, Vector<ValueID, 8>& joins)
{
    ValueID start = stripSteps(index, value, StepsTaken::Down).value;
    if (start == counter || joins.contains(start))
        return true;
    if (!start || !index.isParameter(start) || joins.size() == 8)
        return false;
    BlockID block = index.blockOf(start);
    if (index.isLoopHead(block) || index.isEnteredByHandlerEdge(block) || !index.isInLoop(block, loop))
        return false;
    joins.append(start);
    uint32_t place = start.index() - index.block(block).firstParameter().index();
    return std::ranges::all_of(walker.incomingEdgesOf(index, block), [&](const BodyIncomingEdge& edge) {
        std::span<const ValueID> arguments = edge.target.arguments();
        return place < arguments.size() && isLowered(walker, index, loop, counter, arguments[place], joins);
    });
}

ValueID findLoweredCounterUnder(Walker& walker, const BodyIndex& index, ValueID value)
{
    static constexpr OpcodeSet stepsDown { Opcode::Decrement, Opcode::Sub };
    if (!index.holdsAnyOf(stepsDown))
        return { };
    ValueID counter = stripSteps(index, value, StepsTaken::Both).value;
    if (!counter || !index.isParameter(counter))
        return { };
    BlockID head = index.blockOf(counter);
    if (!index.isLoopHead(head) || index.isEnteredByHandlerEdge(head))
        return { };
    LoopID loop = index.loopOf(head);
    uint32_t place = counter.index() - index.block(head).firstParameter().index();
    bool isStepped = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, head)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        Vector<ValueID, 8> joins;
        if (place >= arguments.size() || !isLowered(walker, index, loop, counter, arguments[place], joins))
            return { };
        isStepped |= index.stripCasts(arguments[place]) != counter;
    }
    return isStepped ? counter : ValueID();
}

static bool isLoweredCounterBelowLength(Walker& walker, const BodyIndex& index, ValueID text, ValueID position)
{
    text = textAsAskedAboutInPositionsInTexts(index, text);
    ValueID counter = text ? findLoweredCounterUnder(walker, index, position) : ValueID();
    if (!counter)
        return false;
    BlockID head = index.blockOf(counter);
    int64_t offset = stripSteps(index, position, StepsTaken::Both).offset;
    uint32_t place = counter.index() - index.block(head).firstParameter().index();
    bool isEntered = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, head)) {
        if (index.isInLoop(edge.from, index.loopOf(head)))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        std::optional<LengthOfTextAsWritten> start = place < arguments.size() ? findLengthOfTextAsWritten(walker.program(), index, arguments[place]) : std::nullopt;
        if (!start || start->text != text || start->distanceBelowLength - offset < 1)
            return false;
        isEntered = true;
    }
    return isEntered;
}

namespace {

struct CharacterReadAsWritten {
    ValueID text;
    ValueID position;
};

} // anonymous namespace

static std::optional<CharacterReadAsWritten> findCharacterReadAsWritten(Walker& walker, const BodyValue& read)
{
    CharacterReadAsWritten found;
    switch (read.opcode()) {
    case Opcode::Call:
        if (read.argumentsOperands().size() != 1)
            return std::nullopt;
        found = { read.thisValueOperand(), read.argumentsOperands()[0] };
        break;
    case Opcode::ElemGet:
        found = { read.objectOperand(), read.propertyOperand() };
        break;
    default:
        return std::nullopt;
    }

    Reference built = walker.referenceOf(found.position);
    if (!built || !built.isRecord() || !walker.types().at(walker.typeOf(built)).isInteger())
        return std::nullopt;
    return found;
}

bool testOfBodyHoldsPositionOfCharacterReadBelowLength(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<CharacterReadAsWritten> written = findCharacterReadAsWritten(walker, read);
    return written && testOfBodyHoldsPositionBelowLengthOfText(walker, index, index.blockOf(read.id()), written->text, written->position);
}

bool readsCharacterBeforeTestedNumber(Walker& walker, const BodyValue& read)
{
    std::optional<CharacterReadAsWritten> written = findCharacterReadAsWritten(walker, read);
    const BodyIndex& index = walker.currentBodyIndex();
    return written && bodyTestHoldsNumberAfterPosition(walker, index, index.blockOf(read.id()), written->text, written->position);
}

bool readsCharacterUnderCounterFromAbove(Walker& walker, const BodyValue& read)
{
    std::optional<CharacterReadAsWritten> written = findCharacterReadAsWritten(walker, read);
    const BodyIndex& index = walker.currentBodyIndex();
    return written && findLoweredCounterUnder(walker, index, written->position) && !isLoweredCounterBelowLength(walker, index, written->text, written->position);
}

bool readsCharacterUnderLoweredCounter(Walker& walker, const BodyValue& read)
{
    std::optional<CharacterReadAsWritten> written = findCharacterReadAsWritten(walker, read);
    return written && isLoweredCounterBelowLength(walker, walker.currentBodyIndex(), written->text, written->position);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
