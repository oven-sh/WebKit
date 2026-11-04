/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Guesses, for numbers: tests of the kind of a number in a row, the stretch with every proof bound, ONE other arm with the values as they stand, and a block where the two meet.
// At a suspension: no stretch holds an operation that may suspend. At a throw: either arm throws to the handler of the input block as any block does.
// Withheld (Guesses, TypedOperations): nothing is built, and each operation takes the form that the chain gives it.

#include "config.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NumberTestedInArms {
    Reference value;
    TypeID tested;
    // The values of the body under which the stretch takes it.
    Vector<ValueID, 2> names;
    Vector<Reference, 2> boundBefore;
    Reference proven;
};

// An arm that has come through the stretch.
struct ArmOfNumbers {
    BlockID lastBlock;
    Vector<Reference, 8> values;
};

} // anonymous namespace

// What the end of the block finds by its definition stays behind every stretch: a test whose outcome the edges hand on as proofs that somebody reads, and a relation by whose record the edges narrow.
// The looks at loops count on both. Whoever walks a stretch has asked for no operation behind its end.
uint32_t limitOfStretchOfNumbers(const Walker& walker, const BodyBlock& block)
{
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t limit = std::min(block.numberOfValues(), walker.endOfStretchBeingWalked());
    auto keepBehindStretches = [&](ValueID value) {
        if (value && value >= block.firstValue() && block.holds(value))
            limit = std::min(limit, value.index() - block.firstValue().index());
    };
    BodyTerminator end = block.terminator();
    if (end.isConditionalBranch()) {
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            if (!result.parameter || index.useCount(result.parameter))
                keepBehindStretches(result.test);
        });
    }
    keepBehindStretches(relationThatNarrowsBehind(walker, end));
    return limit;
}

Reference buildTestOfNumber(Walker& walker, Reference value, TypeID tested, Origin origin)
{
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(tested);
    return walker.build(WTF::move(test));
}

// Where the test has passed.
Reference buildProofOfNumber(Walker& walker, Reference value, Reference test, Origin origin)
{
    Draft narrowed(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(narrowed);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(test);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    return walker.build(WTF::move(narrowed));
}

static bool isReturnedAtOnceInNumberArms(const BodyIndex& index, BlockID block, ValueID value)
{
    BodyTerminator end = index.terminator(block);
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) == value;
}

// As at any join, what both arms bring under one name keeps it. Any other value that is read behind the stretch gets a parameter of the block where they meet, under the join of its two types.
static void letArmsOfNumbersMeet(Walker& walker, std::span<const ArmOfNumbers> arms, std::span<const ValueID> valuesMentionedLater, BlockID meetingBlock)
{
    Code& code = walker.code();
    Vector<size_t, 8> positionsOfParameters;
    for (size_t position = 0; position < valuesMentionedLater.size() && !arms.empty(); ++position) {
        Reference one = arms.front().values[position];
        Reference other = arms.back().values[position];
        Reference met = one;
        if (one != other) {
            TypeID type = joinOf(walker.types(), walker.facts().families(), code.at(one.id()).type, code.at(other.id()).type);
            Representation forms[] { walker.representationOf(one), walker.representationOf(other) };
            met = walker.addParameter(meetingBlock, type, walker.formOfParameter(type, forms));
            positionsOfParameters.append(position);
        }
        walker.bind(valuesMentionedLater[position], met);
    }

    Vector<Reference, 8> arguments;
    for (const ArmOfNumbers& arm : arms) {
        arguments.shrink(0);
        for (size_t position : positionsOfParameters)
            arguments.append(arm.values[position]);
        walker.continueIn(arm.lastBlock);
        walker.jumpTo(meetingBlock, arguments.span());
        if (walker.hasFailed())
            return;
    }
    walker.continueIn(meetingBlock);
}

bool walkStretchBehindTestsOfNumbers(Walker& walker, std::span<const TestOfNumber> tests, BlockID inputBlock, uint32_t begin, uint32_t end, BlockFrequency ofOtherArm)
{
    if (begin >= end || walker.isUnreachable() || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::TypedOperations))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    uint32_t first = index.block(inputBlock).firstValue().index() + begin;
    uint32_t size = end - begin;

    // `x * x`: one value is tested once, under whichever names the stretch takes it.
    Vector<NumberTestedInArms, 8> numbers;
    for (const TestOfNumber& test : tests) {
        Reference value = walker.referenceOf(test.input);
        if (!value || !value.isValue() || !test.tested)
            return false;
        TypeID proven = walker.typeOf(value);
        if (types.isSubset(proven, test.tested))
            continue;
        if (!types.overlap(proven, test.tested))
            return false;
        bool isListed = numbers.containsIf([&](const NumberTestedInArms& listed) {
            return listed.value == value;
        });
        if (!isListed)
            numbers.append(NumberTestedInArms { value, test.tested, { }, { }, { } });
    }
    if (numbers.isEmpty())
        return false;

    Vector<uint32_t, 32> mentions;
    mentions.fill(0, size);
    for (uint32_t place = 0; place < size; ++place) {
        index.value(ValueID(first + place)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand.index() >= first) {
                ++mentions[operand.index() - first];
                return;
            }
            Reference taken = walker.referenceOf(operand);
            for (NumberTestedInArms& number : numbers) {
                if (number.value == taken && !number.names.contains(operand))
                    number.names.append(operand);
            }
        });
    }
    Vector<ValueID, 8> valuesMentionedLater;
    for (uint32_t place = 0; place < size; ++place) {
        uint32_t uses = index.useCount(ValueID(first + place));
        if (uses == BodyIndex::maxCountedUses || uses > mentions[place])
            valuesMentionedLater.append(ValueID(first + place));
    }
    bool takesInCallReturnedAtOnce = isReturnedAtOnceInNumberArms(index, inputBlock, ValueID(first + size - 1));

    Origin origin = walker.originOf(index.value(ValueID(first)));
    origin.isTailCall = false;

    // None of the tests writes anything, so what held in front of the first holds wherever one fails.
    BlockID otherBlock;
    for (NumberTestedInArms& number : numbers) {
        Reference test = buildTestOfNumber(walker, number.value, number.tested, origin);
        if (!test)
            break;
        BlockID passed = walker.addBlock();
        if (!otherBlock)
            otherBlock = walker.addBlock(ofOtherArm);
        walker.branch(test, passed, otherBlock);
        walker.continueIn(passed);
        number.proven = buildProofOfNumber(walker, number.value, test, origin);
        if (!number.proven)
            break;
    }
    if (walker.hasFailed() || !otherBlock)
        return true;
    BlockID blockBehindTests = walker.isUnreachable() ? BlockID() : walker.currentBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.noteGuessOpened();

    Vector<ArmOfNumbers, 2> arms;
    auto endArm = [&] {
        if (walker.isUnreachable())
            return;
        ArmOfNumbers arm { walker.currentBlock(), { } };
        for (ValueID mentioned : valuesMentionedLater) {
            Reference built = walker.referenceOf(mentioned);
            if (!built || !built.isRecord())
                return walker.fail("an arm behind tests of numbers has no value for what is read behind it"_s);
            arm.values.append(built);
        }
        arms.append(WTF::move(arm));
    };

    walker.continueIn(otherBlock);
    {
        Walker::ClosedArm otherArm(walker);
        walker.walkStretch(inputBlock, begin, end);
        if (takesInCallReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        endArm();
    }
    if (walker.hasFailed())
        return true;

    if (blockBehindTests) {
        walker.continueIn(blockBehindTests);
        for (NumberTestedInArms& number : numbers) {
            for (ValueID name : number.names) {
                number.boundBefore.append(walker.referenceBoundTo(name));
                walker.bind(name, number.proven);
            }
        }
        walker.walkStretch(inputBlock, begin, end);
        if (takesInCallReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        endArm();
        for (NumberTestedInArms& number : numbers) {
            for (size_t place = 0; place < number.names.size(); ++place)
                walker.bind(number.names[place], number.boundBefore[place]);
        }
    }
    if (walker.hasFailed())
        return true;

    // As where the arms of a guess meet: what the last arm has bound to a value that the body never mentions again is the proof of one arm.
    for (uint32_t place = 0; place < size; ++place) {
        if (arms.isEmpty() || !valuesMentionedLater.contains(ValueID(first + place)))
            walker.bind(ValueID(first + place), Reference());
    }
    letArmsOfNumbersMeet(walker, arms.span(), valuesMentionedLater.span(), meetingBlock);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
