/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// BuiltinCalls and TypedOperations: `while (pos < text.length) { .. text.charCodeAt(pos) .. }`. The program's own test has passed, so the position lies below the length of the text: the read asks no bounds and gives no NaN.
// The question is asked of the body, not of what was built. A text never changes its length, and both are values of the body, so what an edge has shown holds in all that the block dominates which the edge alone enters.
// So the looks at loops and the walk get one answer from one function, whatever the walk has made of the test: a constant, a parameter of two arms, the bounds of a piece.
// It says nothing of the lower end, nor of what the two values are. Whoever asks has proven the text a string that is no object, and the position an integer that is not negative, by its own types.
// At a suspension and at a throw: nothing of its own. Withheld: AOTWalkStringCharacters.cpp goes by the names and the ranges alone, and the look by the row.

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

// ---- What a relation says

// `bound`: the length of a text less `distanceBelowLength`, which is not negative. Both operands are integers, so a relation that has failed says the opposite.
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
    // Behind its casts.
    ValueID text;
    int64_t distanceBelowLength { 0 };
};

// The test has come out so that `position` lies below the length of `text`, given that the two are what the asker has proven. It holds in the blocks with these numbers in the order of the dominator tree.
struct TestOfPositionInText {
    // Both behind their casts.
    ValueID position;
    ValueID text;
    uint32_t firstDominatorNumber { 0 };
    uint32_t lastDominatorNumber { 0 };

    std::tuple<uint32_t, uint32_t, uint32_t> order() const { return { position.index(), text.index(), firstDominatorNumber }; }
};

// They follow from the program alone. Sorted by order(). Of two tests of the same two values of which one holds wherever the other does, only the wider is kept, so at most one holds in a block.
struct TestsOfPositionsInTexts final : Walker::KeptForBuild {
    UncheckedKeyHashMap<const BodyIndex*, Vector<TestOfPositionInText>> ofBodies;
};

} // anonymous namespace

// The text of the test is the text of the read where this gives one value for both.
static ValueID textAsAskedAboutInPositionsInTexts(const BodyIndex& index, ValueID text)
{
    return index.valueBehindCasts(text);
}

// `text.length`, `text.length - 1`. Of whatever object: the asker knows what `text` is.
static std::optional<LengthOfTextAsWritten> findLengthOfTextAsWritten(const Program& program, const BodyIndex& index, ValueID bound)
{
    LengthOfTextAsWritten found;
    bound = index.valueBehindCasts(bound);
    if (bound && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Sub) {
        BodyValue difference = index.value(bound);
        ValueID less = index.valueBehindCasts(difference.operand(1));
        if (!less || index.isParameter(less) || index.opcodeOf(less) != Opcode::Integer)
            return std::nullopt;
        found.distanceBelowLength = index.value(less).as<Opcode::Integer>().integer();
        if (found.distanceBelowLength < 0)
            return std::nullopt;
        bound = index.valueBehindCasts(difference.operand(0));
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
            ValueID position = index.valueBehindCasts(relation.operand(placeOfPosition));
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

// ---- The one question

bool testOfBodyHoldsPositionBelowLengthOfText(Walker& walker, const BodyIndex& index, BlockID block, ValueID text, ValueID position)
{
    text = textAsAskedAboutInPositionsInTexts(index, text);
    position = index.valueBehindCasts(position);
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

// ---- For the walk

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

    // Of a number that is no integer the builder reads at what is left of it, which is another value than the test has seen.
    Reference built = walker.referenceOf(position);
    if (!built || !built.isRecord() || !walker.types().at(walker.typeOf(built)).isInteger())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    return testOfBodyHoldsPositionBelowLengthOfText(walker, index, index.blockOf(read.id()), text, position);
}

// ---- For the looks at loops

// The code in line that every row for a call of the built-in names. Nothing: they differ, or one has none.
static std::optional<InlineBuiltin> codeInLineOfCallsInPositionsInTexts(const BuiltinTable& table, BuiltinID builtin)
{
    std::optional<InlineBuiltin> found;
    for (const BuiltinOverload& row : table.overloadsOf(builtin)) {
        if (row.mode != BuiltinCallMode::Call)
            continue;
        std::optional<InlineBuiltin> ofRow = row.code.implementation.inlineBuiltin;
        if (!ofRow || (found && *found != *ofRow))
            return std::nullopt;
        found = ofRow;
    }
    return found;
}

TypeID typeOfReadInsideTextAhead(Walker& walker, const BodyIndex& index, const BodyValue& call, const TypesOfInputs& types)
{
    if (!walker.has(Mechanism::BuiltinCalls))
        return { };
    // wouldCallBuiltin() reads the body that the walk stands in.
    if (&index != &walker.currentBodyIndex() || call.opcode() != Opcode::Call || call.isDoneInPlace() || call.argumentsOperands().size() != 1)
        return { };

    TypeArena& arena = walker.types();
    TypeID typeOfCallee = types(call.calleeOperand());
    if (!typeOfCallee)
        return { };
    Type callee = arena.at(typeOfCallee);
    BuiltinID builtin = callee.isObject() ? singleBuiltinOf(arena, callee) : BuiltinID();
    if (!builtin)
        return { };
    const BuiltinTable& table = walker.program().builtins();
    std::optional<InlineBuiltin> codeInLine = codeInLineOfCallsInPositionsInTexts(table, table.canonicalID(builtin));
    if (codeInLine != InlineBuiltin::StringCharCodeAt && codeInLine != InlineBuiltin::StringCodePointAt)
        return { };

    ValueID text = call.thisValueOperand();
    ValueID position = call.argumentsOperands()[0];
    TypeID typeOfText = text ? types(text) : TypeID();
    TypeID typeOfPosition = types(position);
    if (!typeOfText || !typeOfPosition || !isProvenString(walker, typeOfText))
        return { };
    Type integer = arena.at(typeOfPosition);
    if (!integer.isInteger() || integer.integers.minimum < 0)
        return { };
    if (!testOfBodyHoldsPositionBelowLengthOfText(walker, index, index.blockOf(call.id()), text, position))
        return { };
    if (wouldCallBuiltin(walker, call, types) != BuiltinCallAskedAbout::ByOneRow)
        return { };
    return arena.integer(Range { 0, codeInLine == InlineBuiltin::StringCharCodeAt ? 0xFFFF : 0x10FFFF });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
