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

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, Opcode, const BodyValue&, const InputsOfTypedOperation&);

// Closed, `x === x` is a call of a stub: about twenty instructions. Behind the test it is a compare of doubles or nothing at all.
static constexpr uint32_t weightOfComparisonWithItselfInNumberArms = 2;
// One operator is built typed. RULINGS.md, numbers, Q4: whether a closed operator counts for two is tried as an A | B. Where it does, this is 2.
static constexpr uint32_t weightOfBitOperatorInNumberArms = 1;
// Closed, the read lets the object leave, so the object is made at every call: an allocation and a call of a stub against one test.
static constexpr uint32_t weightOfReadOfArgumentsInNumberArms = 2;

static constexpr OpcodeSet operatorsWhoseClosedFormCallsStubInNumberArms = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise });

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
        if (!valuesMentionedLater.contains(ValueID(first + place)))
            walker.bind(ValueID(first + place), Reference());
    }
    letArmsOfNumbersMeet(walker, arms.span(), valuesMentionedLater.span(), meetingBlock);
    return true;
}

// ---- `x !== x`

// Only NaN differs from itself, so whatever is no number equals itself. The test is no guess: both sides are right.
static bool compareValueWithItselfBehindTestOfNumber(Walker& walker, const BodyValue& comparison)
{
    if (comparison.opcode() != Opcode::StrictEq || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable())
        return false;
    ValueID left = comparison.operand(OperandIndex::StrictEq::left);
    ValueID right = comparison.operand(OperandIndex::StrictEq::right);
    Reference value = walker.referenceOf(left);
    if (!value || !value.isValue() || walker.referenceOf(right) != value)
        return false;
    Type type = walker.types().at(walker.typeOf(value));
    if (!type.isJavaScriptValue() || !type.mayBeNaN() || type.isNumber() || type.mayBeUnspecified() || type.primitives & Primitive::Absent)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(comparison.id()));
    uint32_t place = comparison.id().index() - block.firstValue().index();
    if (place >= limitOfStretchOfNumbers(walker, block))
        return false;
    if (!shouldOpenGuess(walker, value, TypeTable::number, true, weightOfComparisonWithItselfInNumberArms))
        return false;

    Origin origin = walker.originOf(comparison);
    origin.isTailCall = false;
    Reference isNumber = buildTestOfNumber(walker, value, TypeTable::number, origin);
    if (!isNumber)
        return true;
    BlockID onNumbers = walker.addBlock();
    BlockID onOthers = walker.addBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.branch(isNumber, onNumbers, onOthers);
    walker.noteGuessOpened();
    Reference met = walker.addParameter(meetingBlock, TypeTable::boolean, walker.formOfParameter(TypeTable::boolean, { }));

    walker.continueIn(onOthers);
    Draft yes(Opcode::Boolean, origin);
    yes.setBoolean(true);
    Reference equalsItself = walker.constant(WTF::move(yes));
    if (!equalsItself)
        return true;
    walker.jumpTo(meetingBlock, singleElementSpan(equalsItself));

    walker.continueIn(onNumbers);
    Reference number = buildProofOfNumber(walker, value, isNumber, origin);
    Reference leftBefore = walker.referenceBoundTo(left);
    Reference rightBefore = walker.referenceBoundTo(right);
    walker.bind(left, number);
    walker.bind(right, number);
    walker.walkStretch(block.id(), place, place + 1);
    Reference answer = walker.isUnreachable() ? Reference() : walker.referenceOf(comparison.id());
    walker.bind(left, leftBefore);
    walker.bind(right, rightBefore);
    if (walker.hasFailed())
        return true;
    if (answer)
        walker.jumpTo(meetingBlock, singleElementSpan(answer));
    else
        walker.continueIn(meetingBlock);
    if (!walker.isUnreachable())
        walker.bind(comparison.id(), met);
    return true;
}

// ---- The low bits of a boxed operand

// `step(s) | 0`, where an integer is expected of what the call gives and nothing is proven. A number that is no int32 is a number still, and the conversion gives an int32 whatever number it takes:
// so only what is no number goes by the operator as written. An int32 goes on as it is, any other number through ToInt32 in a rare block, and the two meet as an int32 in front of the operator.
static bool takeLowBitsOfBoxedOperand(Walker& walker, const BodyValue& operation)
{
    if (!allBitwiseOperators.contains(operation.opcode()) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || walker.isUnreachable())
        return false;
    TypeArena& types = walker.types();

    // Every other operand is taken by a typed form as it is.
    ValueID tested;
    Reference boxed;
    bool takesSeveralUnknown = false;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        Reference reference = walker.referenceOf(operand);
        TypeID type = walker.typeOf(reference);
        if (type && isNumericOperand(types, type))
            return;
        takesSeveralUnknown |= boxed && boxed != reference;
        tested = operand;
        boxed = reference;
    });
    if (!boxed || !boxed.isValue() || takesSeveralUnknown || walker.representationOf(boxed) != Representation::JSValue)
        return false;
    Type type = types.at(walker.typeOf(boxed));
    if (!type.isJavaScriptValue() || !(type.primitives & Primitive::Integer) || !(type.primitives & Primitive::Double) || type.mayBeUnspecified())
        return false;

    // Of what is written to be a number nothing says that it is mostly an int32.
    ExpectationOfValue expectation = findExpectation(walker, tested);
    if (!expectation.type || !types.at(types.withoutNullish(expectation.type)).isInteger())
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(operation.id()));
    uint32_t place = operation.id().index() - block.firstValue().index();
    if (place >= limitOfStretchOfNumbers(walker, block))
        return false;
    if (!shouldOpenGuess(walker, boxed, TypeTable::int32, expectation.isDeclared, weightOfBitOperatorInNumberArms))
        return false;

    Vector<ValueID, 2> names;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        if (walker.referenceOf(operand) == boxed && !names.contains(operand))
            names.append(operand);
    });
    bool isReadLater = !index.isUnused(operation.id());

    Origin origin = walker.originOf(operation);
    origin.isTailCall = false;
    Reference isInt32 = buildTestOfNumber(walker, boxed, TypeTable::int32, origin);
    if (!isInt32)
        return true;
    BlockID onInt32 = walker.addBlock();
    BlockID onOthers = walker.addBlock(BlockFrequency::Rare);
    BlockID onBits = walker.addBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.branch(isInt32, onInt32, onOthers);
    walker.noteGuessOpened();
    Reference bits = walker.addParameter(onBits, TypeTable::int32, walker.formOfParameter(TypeTable::int32, { }));

    walker.continueIn(onOthers);
    Reference isNumber = buildTestOfNumber(walker, boxed, TypeTable::number, origin);
    if (!isNumber)
        return true;
    BlockID onNumbers = walker.addBlock(BlockFrequency::Rare);
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isNumber, onNumbers, asWritten);

    Vector<ArmOfNumbers, 2> arms;
    auto endArm = [&] {
        if (walker.isUnreachable())
            return;
        ArmOfNumbers arm { walker.currentBlock(), { } };
        if (isReadLater) {
            Reference built = walker.referenceOf(operation.id());
            if (!built || !built.isRecord())
                return walker.fail("an arm of a bit operator has no value for what is read behind it"_s);
            arm.values.append(built);
        }
        arms.append(WTF::move(arm));
    };

    walker.continueIn(asWritten);
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm();
    }
    if (walker.hasFailed())
        return true;

    walker.continueIn(onNumbers);
    if (Reference number = buildProofOfNumber(walker, boxed, isNumber, origin)) {
        Draft conversion(Opcode::ToInt32, origin);
        walker.chooseConservativeFields(conversion);
        conversion.setValueOperand(number);
        if (Reference lowBits = walker.build(WTF::move(conversion)))
            walker.jumpTo(onBits, singleElementSpan(lowBits));
    }
    if (walker.hasFailed())
        return true;
    walker.continueIn(onInt32);
    if (Reference integer = buildProofOfNumber(walker, boxed, isInt32, origin))
        walker.jumpTo(onBits, singleElementSpan(integer));
    if (walker.hasFailed())
        return true;

    walker.continueIn(onBits);
    Vector<Reference, 2> boundBefore;
    for (ValueID name : names) {
        boundBefore.append(walker.referenceBoundTo(name));
        walker.bind(name, bits);
    }
    walker.walkStretch(block.id(), place, place + 1);
    endArm();
    for (size_t position = 0; position < names.size(); ++position)
        walker.bind(names[position], boundBefore[position]);
    if (walker.hasFailed())
        return true;

    ValueID given = operation.id();
    letArmsOfNumbersMeet(walker, arms.span(), isReadLater ? std::span<const ValueID>(&given, 1) : std::span<const ValueID>(), meetingBlock);
    return true;
}

// ---- `arguments[k]` with any key

static ValueID valueBehindConversionsInNumberArms(const Code& code, ValueID value)
{
    while (conversionsOfForm.contains(code.at(value).opcode))
        value = code.at(value).valueOperand();
    return value;
}

// Above the object stands Object.prototype alone. The run time refuses every new name on it, but those that a test run has opened: WriterOfLinkLayouts::objectPrototypeMayGainName().
// An accessor under such a name is handed what the read is asked of, and what it stores into a copy is lost to the next read of the object.
static bool accessorMayComeToStandAboveArgumentsInNumberArms(const Program& program)
{
    const BuiltinTable& builtins = program.builtins();
    BuiltinID objectPrototype = builtins.canonicalID(Builtins::ObjectPrototype);
    if (!builtins.at(objectPrototype).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || !builtins.openNamesOf(objectPrototype).empty())
        return true;
    return std::ranges::contains(program.openedNames, objectPrototype, &OpenedName::holder);
}

// Nothing but this code has the object, and it is as it was made. A key that names an argument is answered out of the list of the arguments, and names the object no more.
// Any other key reads a COPY of the object, made for that one read in an unlikely block: nobody gets to hold what the read is asked of. So nothing names the object itself, and the sweep drops it.
// The test is no guess: both sides are right. Of a function that writes no parameters, in strict code: there every element lies in the list.
static bool readArgumentsUnderAnyKey(Walker& walker, const BodyValue& read)
{
    if (read.opcode() != Opcode::ElemGet || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis) || walker.isUnreachable())
        return false;
    if (accessorMayComeToStandAboveArgumentsInNumberArms(walker.program()))
        return false;
    ValueID objectAsWritten = read.objectOperand();
    ValueID keyAsWritten = read.propertyOperand();
    Reference object = walker.referenceOf(objectAsWritten);
    Reference key = walker.referenceOf(keyAsWritten);
    if (!object || !object.isValue() || !key || !key.isValue() || objectAsWritten == keyAsWritten)
        return false;
    const Code& code = walker.code();
    const Operation& made = code.at(object.id());
    if (made.opcode != Opcode::CreateUnmappedArguments || !made.parametersOperands().empty())
        return false;

    // What the state holds of the object is what the allocation lists, so an allocation that lists the same makes the same object.
    Reference list = listStandingForFreshArguments(walker, object);
    Reference count = lengthOfFreshArguments(walker, object);
    if (!list || !list.isRecord() || !count || !count.isRecord())
        return false;
    if (valueBehindConversionsInNumberArms(code, list.id()) != valueBehindConversionsInNumberArms(code, made.restOperand()))
        return false;
    if (valueBehindConversionsInNumberArms(code, count.id()) != valueBehindConversionsInNumberArms(code, made.countOperand()))
        return false;

    TypeArena& types = walker.types();
    TypeID typeOfKey = walker.typeOf(key);
    if (types.at(typeOfKey).isInteger() || !types.at(typeOfKey).isJavaScriptValue() || !types.overlap(typeOfKey, TypeTable::length))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    uint32_t place = read.id().index() - block.firstValue().index();
    if (place >= limitOfStretchOfNumbers(walker, block))
        return false;
    if (!shouldOpenGuess(walker, key, TypeTable::length, true, weightOfReadOfArgumentsInNumberArms))
        return false;
    bool isReadLater = !index.isUnused(read.id());

    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    Reference namesArgument = buildTestOfNumber(walker, key, TypeTable::length, origin);
    if (!namesArgument)
        return true;
    BlockID outOfList = walker.addBlock();
    BlockID offCopy = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meetingBlock = walker.addBlock();
    walker.branch(namesArgument, outOfList, offCopy);
    walker.noteGuessOpened();

    Vector<ArmOfNumbers, 2> arms;
    auto endArm = [&] {
        if (walker.isUnreachable())
            return;
        ArmOfNumbers arm { walker.currentBlock(), { } };
        if (isReadLater) {
            Reference built = walker.referenceOf(read.id());
            if (!built || !built.isRecord())
                return walker.fail("an arm of a read of arguments has no value for what is read behind it"_s);
            arm.values.append(built);
        }
        arms.append(WTF::move(arm));
    };

    walker.continueIn(offCopy);
    {
        Walker::ClosedArm closedArm(walker);
        Draft allocation(Opcode::CreateUnmappedArguments, origin);
        allocation.standsForNoValueOfInput = true;
        walker.chooseConservativeFields(allocation);
        allocation.setCountOperand(count);
        allocation.setRestOperand(list);
        if (Reference copy = walker.build(WTF::move(allocation))) {
            walker.bind(objectAsWritten, copy);
            walker.walkStretch(block.id(), place, place + 1);
            endArm();
        }
    }
    if (walker.hasFailed())
        return true;

    walker.continueIn(outOfList);
    Reference position = buildProofOfNumber(walker, key, namesArgument, origin);
    Reference keyBefore = walker.referenceBoundTo(keyAsWritten);
    walker.bind(keyAsWritten, position);
    walker.walkStretch(block.id(), place, place + 1);
    endArm();
    walker.bind(keyAsWritten, keyBefore);
    if (walker.hasFailed())
        return true;

    ValueID given = read.id();
    letArmsOfNumbersMeet(walker, arms.span(), isReadLater ? std::span<const ValueID>(&given, 1) : std::span<const ValueID>(), meetingBlock);
    return true;
}

bool openGuessOfNumbers(Walker& walker, const BodyValue& operation)
{
    return compareValueWithItselfBehindTestOfNumber(walker, operation) || takeLowBitsOfBoxedOperand(walker, operation);
}

bool openGuessAtReadOfArguments(Walker& walker, const BodyValue& operation)
{
    return readArgumentsUnderAnyKey(walker, operation);
}

bool closedOperatorCallsStub(Walker& walker, const BodyValue& operation, const TypesOfInputs& inClosedArm)
{
    if (!operatorsWhoseClosedFormCallsStubInNumberArms.contains(operation.opcode()))
        return false;
    return formAllowedForTypedOperator(walker, operation.opcode(), operation, InputsOfTypedOperation(walker, &inClosedArm)) == OperatorForm::Generic;
}

// ---- What RULINGS.md has given to others

TypeID resultOfCallByArgumentsAtHand(const Walker&, TypeLevel, uint32_t)
{
    return { };
}

Reference foldCallInRealm(Walker&, const CallByOneRow&)
{
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
