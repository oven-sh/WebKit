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
    Vector<const NumberReadBeforeArms*, 1> reads;
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

static Reference buildTestOfTaggedInteger(Walker& walker, Reference value, Origin origin)
{
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(TypeTable::int32);
    test.setMayRejectWholeDoubles(true);
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

static void appendNullishArms(Walker& walker, Reference value, Origin origin, Vector<Walker::JoinArm, 4>& arms)
{
    struct NumberOfNullish {
        TypeID type;
        double number;
    };
    for (const NumberOfNullish& nullish : { NumberOfNullish { TypeTable::null, 0 }, NumberOfNullish { TypeTable::undefined, PNaN } }) {
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        Reference test = buildTestOfNumber(walker, value, nullish.type, origin);
        if (!test || walker.hasFailed())
            return;
        BlockID isNullish = walker.addBlock(BlockFrequency::Rare);
        BlockID isOther = walker.addBlock(BlockFrequency::Rare);
        walker.branch(test, isNullish, isOther);
        walker.continueIn(isNullish);
        if (!walker.isUnreachable())
            arms.append({ walker.currentBlock(), { walker.constant(PrimitiveKnownToFolding::number(nullish.number)) } });
        walker.continueIn(isOther);
    }
    if (!walker.isUnreachable() && !walker.hasFailed())
        walker.restartIteration();
}

bool isConvertedToNumber(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet products { Opcode::Mul, Opcode::Div, Opcode::Sub };
    const BodyIndex& index = walker.currentBodyIndex();
    bool isReadAgain = std::ranges::any_of(index.block(index.blockOf(read.id())).values(), [&](BodyValue other) {
        return other.opcode() == Opcode::PropGet && other.id() != read.id() && !other.isFreeName() && other.key() == read.key();
    });
    if (isReadAgain)
        return false;
    ValueID value = read.id();
    ValueID user = index.onlyUsingValue(value);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        value = user;
        user = index.onlyUsingValue(user);
    }
    if (!user || products.contains(index.opcodeOf(user)))
        return !!user;
    if (index.opcodeOf(user) != Opcode::Add)
        return false;
    ValueID beside = index.value(user).operand(index.value(user).operand(0) == value ? 1 : 0);
    if (Reference built = walker.referenceOf(beside); built && built.isRecord())
        return walker.types().at(walker.typeOf(built)).isNumber();
    ValueID other = index.stripCasts(beside);
    if (!other || index.isParameter(other) || index.opcodeOf(other) != Opcode::PropGet || index.blockOf(other) != index.blockOf(read.id()))
        return false;
    BodyValue otherRead = index.value(other);
    return !otherRead.isFreeName() && otherRead.key() != read.key() && index.stripCasts(otherRead.objectOperand()) == index.stripCasts(read.objectOperand()) && siteMayAnswerReadWithoutOtherCode(walker, otherRead) && index.onlyUsingValue(other);
}

bool bindNumberOfNullish(Walker& walker, const BodyValue& read, Reference found, Origin origin)
{
    if (!found.isValue() || walker.representationOf(found) != Representation::JSValue || !walker.types().overlap(walker.typeOf(found), TypeTable::number) || walker.types().isSubset(walker.typeOf(found), TypeTable::number))
        return false;
    walker.recordSpeculation();
    Reference isNumber = buildTestOfNumber(walker, found, TypeTable::number, origin);
    if (!isNumber || walker.hasFailed() || walker.isUnreachable())
        return true;
    BlockID holdsNumber = walker.addBlock();
    BlockID holdsOther = walker.addBlock(BlockFrequency::Rare);
    Vector<Walker::JoinArm, 4> arms;
    walker.branch(isNumber, holdsNumber, holdsOther);
    walker.continueIn(holdsNumber);
    if (!walker.isUnreachable()) {
        Reference number = buildProofOfNumber(walker, found, isNumber, origin);
        if (!number || walker.hasFailed())
            return true;
        Draft unboxed(Opcode::Unbox, origin);
        unboxed.setOperand(0, number);
        unboxed.representation = Representation::Double;
        Reference unboxedNumber = walker.build(WTF::move(unboxed));
        if (!unboxedNumber || walker.hasFailed())
            return true;
        arms.append({ walker.currentBlock(), { unboxedNumber } });
    }
    walker.continueIn(holdsOther);
    appendNullishArms(walker, found, origin, arms);
    std::array<Reference, 1> met;
    if (walker.hasFailed() || !walker.joinArms(arms.span(), walker.addBlock(), met))
        return true;
    walker.bind(read.id(), met[0]);
    walker.count(StatisticID::SiteReadsOfNullishNumbers);
    return true;
}

bool isTakenByBitOperator(Walker& walker, ValueID value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(value);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    return user && allBitwiseOperators.contains(index.opcodeOf(user));
}

bool bindBitsOfPrimitive(Walker& walker, const BodyValue& read, Reference found, std::optional<Draft>&& readAgain, Origin origin)
{
    TypeArena& types = walker.types();
    TypeID typeFound = found.isValue() ? walker.typeOf(found) : TypeID();
    if (!typeFound || walker.representationOf(found) != Representation::JSValue || !(types.at(typeFound).primitives & Primitive::Integer) || types.isSubset(typeFound, TypeTable::number))
        return false;
    walker.recordSpeculation();
    Reference test = buildTestOfNumber(walker, found, TypeArena::fromPrimitives(Primitive::NumberLike), origin);
    if (!test || walker.hasFailed() || walker.isUnreachable())
        return true;
    BlockID leaves = walker.addBlock(BlockFrequency::Rare);
    BlockID holdsBits = walker.addBlock();
    walker.branch(test, holdsBits, leaves);
    walker.continueIn(leaves);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(holdsBits);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    Reference primitive = buildProofOfNumber(walker, found, test, origin);
    if (!primitive || walker.hasFailed())
        return true;
    Reference bits = buildOperatorInNumberForms(walker, Opcode::BitOr, primitive, integerInNumberForms(walker, 0), origin);
    if (!bits || walker.hasFailed())
        return true;
    walker.bind(read.id(), bits);
    walker.count(StatisticID::SiteReadsOfBits);
    if (readAgain)
        walker.holdValueRememberedAgainstValueAtSite(primitive, WTF::move(*readAgain));
    return true;
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

bool tryGuardCounterBound(Walker& walker, Draft& relation, ValueID written)
{
    if (!allRelations.contains(relation.opcode) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.canRestartIteration())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    for (uint32_t side : { 0u, 1u }) {
        Reference bound = relation.operand(side);
        Reference counter = relation.operand(1 - side);
        ValueID counted = index.stripCasts(index.value(written).operand(1 - side));
        bool isCounter = counted && index.isParameter(counted) && index.loopOf(index.blockOf(counted)) && index.isLoopHead(index.blockOf(counted)) && counter && types.at(walker.typeOf(counter)).isInteger();
        if (!isCounter || !bound || !bound.isValue() || !types.overlap(walker.typeOf(bound), TypeTable::int32) || types.at(walker.typeOf(bound)).isNumber() || !readsFieldOfIntegers(walker, index.value(written).operand(side)))
            continue;
        Origin origin = relation.origin();
        origin.isTailCall = false;
        Reference isInt32 = buildTestOfNumber(walker, bound, TypeTable::int32, origin);
        if (!isInt32)
            return false;
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        walker.letGuessFailUnasked(failed, origin);
        walker.branch(isInt32, passed, failed);
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            walker.restartIteration();
        walker.continueIn(passed);
        walker.recordSpeculation();
        if (walker.isUnreachable() || walker.hasFailed())
            return false;
        Reference integer = buildProofOfNumber(walker, bound, isInt32, origin);
        if (!integer)
            return false;
        walker.count(StatisticID::IntegerFieldsSpeculated);
        relation.setOperand(side, integer);
        return true;
    }
    return false;
}

static bool isIndexInNumberArmIterations(const BodyIndex& index, ValueID value)
{
    LoopID loop = index.loopOf(index.blockOf(value));
    return loop && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue access) {
            return (access.opcode() == Opcode::ElemGet || access.opcode() == Opcode::ElemSet) && index.stripCasts(access.propertyOperand()) == value;
        });
    });
}

bool tryGuardAddendOfIndex(Walker& walker, Draft& sum, ValueID written)
{
    if ((sum.opcode != Opcode::Add && sum.opcode != Opcode::Sub) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.isInLoop() || !walker.canRestartIteration() || !walker.canBuildIterationGuard())
        return false;
    TypeArena& types = walker.types();
    for (uint32_t side : { 0u, 1u }) {
        Reference addend = sum.operand(side);
        Reference beside = sum.operand(1 - side);
        if (!addend.isValue() || walker.representationOf(addend) != Representation::OptionalInt64 || !types.overlap(walker.typeOf(addend), TypeTable::int32) || types.at(walker.typeOf(addend)).isNumber() || !types.at(walker.typeOf(beside)).isInt32())
            continue;
        if (!isIndexInNumberArmIterations(walker.currentBodyIndex(), written))
            return false;
        Origin origin = sum.origin();
        origin.isTailCall = false;
        Reference isInt32 = buildTestOfNumber(walker, addend, TypeTable::int32, origin);
        if (!isInt32)
            return false;
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        walker.letGuessFailUnasked(failed, origin);
        walker.branch(isInt32, passed, failed);
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            walker.restartIteration();
        walker.continueIn(passed);
        walker.recordSpeculation();
        if (walker.isUnreachable() || walker.hasFailed())
            return false;
        Reference integer = buildProofOfNumber(walker, addend, isInt32, origin);
        if (!integer)
            return false;
        walker.count(StatisticID::IntegerFieldsSpeculated);
        sum.setOperand(side, integer);
        return true;
    }
    return false;
}

static bool isConvertedOnPath(const BodyIndex& index, ValueID value, uint32_t begin, uint32_t end)
{
    ValueID user = index.onlyUsingValue(value);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    return user && user.index() >= begin && user.index() < end && !allEqualities.contains(index.opcodeOf(user));
}

static bool isReturnedAtOnceInNumberArms(const BodyIndex& index, BlockID block, ValueID value)
{
    BodyTerminator end = index.terminator(block);
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) == value;
}

bool walkStretchBehindTestsOfNumbers(Walker& walker, std::span<const TestOfNumber> tests, BlockID inputBlock, uint32_t begin, uint32_t end, BlockFrequency ofOtherArm, EndOfNumberArms ending, std::span<const NumberReadBeforeArms> reads)
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
            numbers.append(NumberTestedInArms { value, test.tested, { }, { }, { }, { } });
    }
    for (const NumberReadBeforeArms& read : reads) {
        size_t listed = numbers.findIf([&](const NumberTestedInArms& number) {
            return number.value == read.found;
        });
        if (listed == notFound) {
            listed = numbers.size();
            numbers.append(NumberTestedInArms { read.found, TypeTable::int32, { }, { }, { }, { } });
        }
        numbers[listed].reads.append(&read);
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

    auto holdAgainstSites = [&](const NumberTestedInArms& number, Reference proven) {
        for (const NumberReadBeforeArms* read : number.reads) {
            if (read->readAgain)
                walker.holdValueRememberedAgainstValueAtSite(proven, Draft(*read->readAgain));
        }
        if (!number.reads.isEmpty())
            noteCheckedRead(walker, number.value, proven);
    };
    size_t operatorsMadeAhead = walker.numberOfHoistedOperators();
    auto bindNumber = [&](NumberTestedInArms& number, Reference proven) {
        for (ValueID name : number.names) {
            number.boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, proven);
        }
        for (const NumberReadBeforeArms* read : number.reads) {
            if (read->read.index() >= first)
                walker.noteOperatorMadeInFrontOfLoop(read->read, proven);
        }
    };
    auto unbindNumbers = [&] {
        for (NumberTestedInArms& number : numbers) {
            for (size_t place = 0; place < number.boundBefore.size(); ++place)
                walker.bind(number.names[place], number.boundBefore[place]);
            number.boundBefore.shrink(0);
        }
        walker.forgetOperatorsMadeAhead(operatorsMadeAhead);
    };

    BlockID otherBlock;
    Reference onlyTest;
    for (NumberTestedInArms& number : numbers) {
        Reference test = number.reads.isEmpty() ? buildTestOfNumber(walker, number.value, number.tested, origin) : buildTestOfTaggedInteger(walker, number.value, origin);
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
        holdAgainstSites(number, number.proven);
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
        if (refused)
            bindNumber(numbers[0], refused);
        for (NumberTestedInArms& number : numbers) {
            if (number.reads.isEmpty() || walker.isUnreachable() || walker.hasFailed())
                continue;
            Reference isNumber = buildTestOfNumber(walker, number.value, TypeTable::number, origin);
            if (!isNumber)
                break;
            BlockID leaves = walker.addBlock(BlockFrequency::Rare);
            BlockID goesOn = walker.addBlock(ofOtherArm);
            bool takesNullish = std::ranges::all_of(number.reads, [&](const NumberReadBeforeArms* read) { return isConvertedOnPath(index, read->read, first, first + size); });
            Vector<Walker::JoinArm, 4> armsOfNumber;
            walker.branch(isNumber, goesOn, leaves);
            walker.continueIn(leaves);
            if (takesNullish)
                appendNullishArms(walker, number.value, origin, armsOfNumber);
            else if (!walker.isUnreachable())
                walker.restartIteration();
            walker.continueIn(goesOn);
            Reference proven = walker.isUnreachable() || walker.hasFailed() ? Reference() : buildProofOfNumber(walker, number.value, isNumber, origin);
            if (!proven)
                break;
            if (!takesNullish)
                holdAgainstSites(number, proven);
            else {
                armsOfNumber.append({ walker.currentBlock(), { proven } });
                std::array<Reference, 1> met;
                if (!walker.joinArms(armsOfNumber.span(), walker.addBlock(ofOtherArm), met))
                    break;
                proven = met[0];
                walker.count(StatisticID::SiteReadsOfNullishNumbers);
            }
            bindNumber(number, proven);
        }
        walker.walkStretch(inputBlock, begin, end);
        if (takesInCallReturnedAtOnce)
            walker.threadReturnOf(inputBlock);
        endArm();
        unbindNumbers();
    }
    if (walker.hasFailed())
        return true;

    if (blockBehindTests) {
        walker.continueIn(blockBehindTests);
        for (NumberTestedInArms& number : numbers)
            bindNumber(number, number.proven);
        walker.walkStretch(inputBlock, begin, end);
        if (takesInCallReturnedAtOnce)
            walker.threadReturnOf(inputBlock);
        endArm();
        unbindNumbers();
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
