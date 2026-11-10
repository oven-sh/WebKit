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

struct TestsOfPositionsInTexts final : Walker::KeptForBuild {
    UncheckedKeyHashMap<const BodyIndex*, Vector<TestOfPositionInText>> ofBodies;
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

static Vector<TestOfPositionInText> collectTestsOfPositionsInTexts(const Program& program, const BodyIndex& index)
{
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
            std::optional<LengthOfTextAsWritten> bound = findLengthOfTextAsWritten(program, index, relation.operand(1 - placeOfPosition));
            if (!bound || !relationHoldsPositionBelowLength(relation.opcode(), holds, placeOfPosition, bound->distanceBelowLength))
                continue;
            ValueID position = index.stripCasts(relation.operand(placeOfPosition));
            if (position)
                tests.append(TestOfPositionInText { position, bound->text, index.dominatorNumber(entered), index.lastDominatorNumberBelow(entered) });
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

bool testOfBodyHoldsPositionBelowLengthOfText(Walker& walker, const BodyIndex& index, BlockID block, ValueID text, ValueID position)
{
    text = textAsAskedAboutInPositionsInTexts(index, text);
    position = index.stripCasts(position);
    if (!text || !position || !index.isReachable(block))
        return false;

    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByPositionsInTexts();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<TestsOfPositionsInTexts>();
    const Vector<TestOfPositionInText>& tests = static_cast<TestsOfPositionsInTexts&>(*kept).ofBodies.ensure(&index, [&] {
        return collectTestsOfPositionsInTexts(walker.program(), index);
    }).iterator->value;

    uint32_t number = index.dominatorNumber(block);
    auto behind = std::ranges::upper_bound(tests, std::tuple(position.index(), text.index(), number), { }, &TestOfPositionInText::order);
    if (behind == tests.begin())
        return false;
    const TestOfPositionInText& last = *std::prev(behind);
    return last.position == position && last.text == text && number <= last.lastDominatorNumber;
}

bool testOfBodyHoldsPositionOfCharacterReadBelowLength(Walker& walker, const BodyValue& read)
{
    ValueID text;
    ValueID position;
    switch (read.opcode()) {
    case Opcode::Call:
        if (read.argumentsOperands().size() != 1)
            return false;
        text = read.thisValueOperand();
        position = read.argumentsOperands()[0];
        break;
    case Opcode::ElemGet:
        text = read.objectOperand();
        position = read.propertyOperand();
        break;
    default:
        return false;
    }

    Reference built = walker.referenceOf(position);
    if (!built || !built.isRecord() || !walker.types().at(walker.typeOf(built)).isInteger())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    return testOfBodyHoldsPositionBelowLengthOfText(walker, index, index.blockOf(read.id()), text, position);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
