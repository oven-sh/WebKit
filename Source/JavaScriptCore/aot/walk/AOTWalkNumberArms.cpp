/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    Vector<ValueID, 2> names;
    Vector<Reference, 2> boundBefore;
    Reference proven;
};

} // anonymous namespace

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

std::optional<uint32_t> findPlaceBehindUpdateOfElement(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet operatorsInsideUpdate = numberProducingOperators | allSums;
    static constexpr OpcodeSet passedInsideUpdate = opcodesThatStateTypes | OpcodeSet { Opcode::Integer, Opcode::Double };
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.opcode() != Opcode::ElemGet || read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words() || walker.hasStringsInBuilders())
        return std::nullopt;
    BodyBlock block = index.block(index.blockOf(read.id()));
    if (walker.inputBlockBeingWalked() != block.id())
        return std::nullopt;
    TypeArena& types = walker.types();
    auto namesSame = [&](ValueID stored, ValueID loaded) {
        if (index.stripCasts(stored) == index.stripCasts(loaded))
            return !!index.stripCasts(stored);
        Reference built = stored < read.id() ? walker.referenceOf(stored) : Reference();
        return built && built.isValue() && built == walker.referenceOf(loaded);
    };
    uint32_t first = block.firstValue().index();
    uint32_t limit = limitOfStretchOfNumbers(walker, block);
    for (uint32_t place = read.id().index() + 1 - first; place < limit; ++place) {
        BodyValue operation = index.value(ValueID(first + place));
        Opcode opcode = operation.opcode();
        if (opcode == Opcode::ElemSet) {
            bool storesWhereItWasRead = namesSame(operation.objectOperand(), read.objectOperand()) && namesSame(operation.propertyOperand(), read.propertyOperand());
            return storesWhereItWasRead && operation.valueOperand() > read.id() ? std::optional<uint32_t>(place + 1) : std::nullopt;
        }
        if (passedInsideUpdate.contains(opcode) && !opcodesThatStateTypes.contains(opcode))
            continue;
        if (!passedInsideUpdate.contains(opcode) && !operatorsInsideUpdate.contains(opcode))
            return std::nullopt;
        bool takesNumbersAlone = true;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            if (operand >= read.id())
                return;
            Reference taken = walker.referenceOf(operand);
            takesNumbersAlone &= taken && taken.isRecord() && types.at(walker.typeOf(taken)).isNumber();
        });
        if (!takesNumbersAlone)
            return std::nullopt;
    }
    return std::nullopt;
}

Reference buildTestOfNumber(Walker& walker, Reference value, TypeID tested, Origin origin)
{
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(tested);
    return walker.build(WTF::move(test));
}

Reference buildProofOfNumber(Walker& walker, Reference value, Reference test, Origin origin, bool passes)
{
    Draft narrowed(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(narrowed);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(test);
    narrowed.setOperandIndex(test.isValue() && walker.code().at(test.id()).opcode == Opcode::ShapeTest ? OperandIndex::ShapeTest::object : OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(passes);
    return walker.build(WTF::move(narrowed));
}

bool tryGuardNumericOperand(Walker& walker, Draft& draft, ValueID written)
{
    if (effectRuleOf(draft.opcode) != EffectRule::Operator || draft.numberOfNamedOperands() != 2)
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.canRestartIteration() || !walker.canBuildIterationGuard() || !walker.hasDirectElementStoreInLoop())
        return false;
    TypeArena& types = walker.types();
    uint32_t asked = types.at(walker.typeOf(draft.operand(0))).isNumber() ? 1 : 0;
    Reference operand = draft.operand(asked);
    TypeID type = walker.typeOf(operand);
    if (!operand.isValue() || !types.at(walker.typeOf(draft.operand(1 - asked))).isNumber() || !types.overlap(type, TypeTable::number) || !types.overlap(type, TypeTable::object))
        return false;
    TypeID expected = findExpectation(walker, walker.currentBodyIndex().value(written).operand(asked)).type;
    if (!expected || !types.at(expected).isNumber())
        return false;
    Origin origin = draft.origin();
    origin.isTailCall = false;
    Reference isNumber = buildTestOfNumber(walker, operand, TypeTable::number, origin);
    if (!isNumber)
        return false;
    BlockID passed = walker.addBlock();
    BlockID failed = walker.addBlock(BlockFrequency::Rare);
    walker.letGuessFailUnasked(failed, origin);
    walker.branch(isNumber, passed, failed);
    walker.continueIn(failed);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(passed);
    walker.recordSpeculation();
    if (walker.isUnreachable() || walker.hasFailed())
        return false;
    Reference number = buildProofOfNumber(walker, operand, isNumber, origin);
    if (!number)
        return false;
    walker.count(StatisticID::NumberOperandsGuardedInIterations);
    draft.setOperand(asked, number);
    return true;
}

static bool isReturnedAtOnceInNumberArms(const BodyIndex& index, BlockID block, ValueID value)
{
    BodyTerminator end = index.terminator(block);
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) == value;
}

bool walkStretchBehindTestsOfNumbers(Walker& walker, std::span<const TestOfNumber> tests, BlockID inputBlock, uint32_t begin, uint32_t end, BlockFrequency ofOtherArm, EndOfNumberArms ending)
{
    if (begin >= end || walker.isUnreachable() || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::TypedOperations))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    uint32_t first = index.block(inputBlock).firstValue().index() + begin;
    uint32_t size = end - begin;

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
    bool mayTakeBranch = ending == EndOfNumberArms::MayTakeBranch && walker.armsMayTakeBranchOf(inputBlock, end, numbers[0].value);
    bool takesInBranch = mayTakeBranch && std::ranges::any_of(numbers, [&](const NumberTestedInArms& number) {
        return walker.armsTakeBranchForProofOf(inputBlock, begin, end, number.value, BitVector());
    });
    auto armKnowsAnswer = [&] {
        Reference answer = mayTakeBranch && !walker.isUnreachable() ? walker.referenceOf(ValueID(first + size - 1)) : Reference();
        TypeID type = answer && answer.isRecord() ? walker.typeOf(answer) : TypeID();
        return type && (type == TypeArena::fromPrimitives(Primitive::True) || type == TypeArena::fromPrimitives(Primitive::False));
    };

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

    BlockID otherBlock;
    Reference onlyTest;
    for (NumberTestedInArms& number : numbers) {
        Reference test = buildTestOfNumber(walker, number.value, number.tested, origin);
        onlyTest = mayTakeBranch && numbers.size() == 1 ? test : Reference();
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
        if (mayTakeBranch && !walker.isUnreachable() && number.proven.isValue()) {
            Reference last = number.value;
            while (Reference checked = walker.state().checkedNameOf(last))
                last = checked;
            if (last != number.proven)
                walker.state().noteCheckedName(last, number.proven);
        }
    }
    if (walker.hasFailed() || !otherBlock)
        return true;
    BlockID blockBehindTests = walker.isUnreachable() ? BlockID() : walker.currentBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.recordSpeculation();

    Vector<Walker::JoinArm, 2> arms;
    auto endArm = [&] {
        if (takesInBranch || armKnowsAnswer())
            walker.threadBranchOf(inputBlock);
        if (walker.isUnreachable())
            return;
        Walker::JoinArm arm { walker.currentBlock(), { } };
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
        Reference refused = onlyTest && !walker.isUnreachable() ? buildProofOfNumber(walker, numbers[0].value, onlyTest, origin, false) : Reference();
        for (ValueID name : refused ? numbers[0].names.span() : std::span<const ValueID>()) {
            numbers[0].boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, refused);
        }
        walker.walkStretch(inputBlock, begin, end);
        if (takesInCallReturnedAtOnce)
            walker.threadReturnOf(inputBlock);
        endArm();
        for (size_t place = 0; place < numbers[0].boundBefore.size(); ++place)
            walker.bind(numbers[0].names[place], numbers[0].boundBefore[place]);
        numbers[0].boundBefore.shrink(0);
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
            walker.threadReturnOf(inputBlock);
        endArm();
        for (NumberTestedInArms& number : numbers) {
            for (size_t place = 0; place < number.names.size(); ++place)
                walker.bind(number.names[place], number.boundBefore[place]);
        }
    }
    if (walker.hasFailed())
        return true;

    for (uint32_t place = 0; place < size; ++place) {
        if (arms.isEmpty() || !valuesMentionedLater.contains(ValueID(first + place)))
            walker.bind(ValueID(first + place), Reference());
    }
    Vector<Reference, 8> met(valuesMentionedLater.size());
    walker.joinArms(arms.span(), meetingBlock, met.mutableSpan());
    for (size_t position = 0; position < met.size() && !arms.isEmpty() && !walker.hasFailed(); ++position)
        walker.bind(valuesMentionedLater[position], met[position]);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
