/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations with UnboxedNumbers, in front of buildTypedOperator(): operators, relations and tests on proven numbers that have a better form than the one their own operands allow.
// Each rule reads the body as written behind an operand, and the proven types. It puts in no guess: where it builds arms, both are right. With Folding: an older value or a constant stands for the operation.
// At a suspension and at a throw: nothing of its own, whatever it builds is pure. Withheld: buildTypedOperator() answers as before.

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "JSGlobalObjectFunctions.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

#if __has_include("AOTWalkNumberSlots.h")
#include "AOTWalkNumberSlots.h"
#endif

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
// AOTWalkNumbers.cpp
bool isTakenAsItIsByFormNumbers(const Type&);

static constexpr OpcodeSet arithmeticInNumberOperators { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow, Opcode::Negate, Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet opcodesOfNumberOperators = arithmeticInNumberOperators | allBitwiseOperators | allEqualities | allRelations
    | OpcodeSet { Opcode::DirectEval, Opcode::LogicalNot, Opcode::ToBoolean };

// The run time evaluates a string, asks the host about an object, and hands anything else back.
static constexpr uint16_t primitivesThatDirectEvalHandsBack = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnySymbol | Primitive::BigInt;

namespace {

struct OperandInNumberOperators {
    ValueID written;
    Reference reference;
    Type type;
    PrimitiveKnownToFolding known;
};

using OperandsInNumberOperators = std::array<OperandInNumberOperators, 2>;

// What stands for a number beside an integer: the answer where every integer compares alike, else the integer that compares as the number does.
struct IntegerInPlaceOfNumber {
    std::optional<bool> answer;
    int64_t bound { 0 };
};

} // anonymous namespace

// False: something was not built, or no run comes here.
static bool readOperandsInNumberOperators(Walker& walker, const BodyValue& value, OperandsInNumberOperators& operands)
{
    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(value.opcode()); ++operandIndex) {
        OperandInNumberOperators& operand = operands[operandIndex];
        operand.written = value.operand(operandIndex);
        operand.reference = operand.written ? walker.referenceOf(operand.written) : Reference();
        TypeID type = operand.reference && operand.reference.isRecord() ? walker.typeOf(operand.reference) : TypeID();
        if (!type || isNeverType(type))
            return false;
        operand.type = walker.types().at(type);
        operand.known = primitiveKnownInNumberForms(walker, operand.reference);
    }
    return true;
}

static bool isKnownIntegerInNumberOperators(PrimitiveKnownToFolding known, int64_t integer)
{
    if (known.kind() != PrimitiveKnownToFolding::Kind::Number || known.asNumber() != static_cast<double>(integer))
        return false;
    return integer || !std::signbit(known.asNumber());
}

static bool isNegativeZeroInNumberOperators(PrimitiveKnownToFolding known)
{
    return known.kind() == PrimitiveKnownToFolding::Kind::Number && !known.asNumber() && std::signbit(known.asNumber());
}

static bool bindInNumberOperators(Walker& walker, const BodyValue& value, Reference result)
{
    if (result)
        walker.bind(value.id(), result);
    return true;
}

// The operation as written on other operands, in the form that those allow.
static bool buildOnOperandsInNumberOperators(Walker& walker, const BodyValue& value, std::span<const Reference> operands)
{
    Draft draft = walker.draftOf(value);
    for (uint32_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
        draft.setOperand(operandIndex, operands[operandIndex]);
    return bindInNumberOperators(walker, value, buildInAllowedFormInNumberForms(walker, WTF::move(draft)));
}

static Origin originInFrontOfNumberOperator(Walker& walker, const BodyValue& value)
{
    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    return origin;
}

// ---- eval of what is no text

static bool bindWhatDirectEvalHandsBack(Walker& walker, const BodyValue& evaluation)
{
    if (!walker.has(Mechanism::Folding))
        return false;
    Reference text = walker.referenceOf(evaluation.textOperand());
    TypeID id = text && text.isRecord() ? walker.typeOf(text) : TypeID();
    if (!id)
        return false;
    Type type = walker.types().at(id);
    if (type.isNever() || !type.hasOnlyPrimitives(primitivesThatDirectEvalHandsBack))
        return false;
    walker.bind(evaluation.id(), text);
    return true;
}

// ---- `!!ok`

// The negation of a relation is not the opposite relation: beside NaN both are false. Two negations of a boolean are the boolean.
static bool bindWhatIsNegatedTwice(Walker& walker, const BodyValue& negation)
{
    if (!walker.has(Mechanism::Folding))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID inner = index.valueBehindCasts(negation.valueOperand());
    if (!inner || index.isParameter(inner) || index.opcodeOf(inner) != Opcode::LogicalNot)
        return false;
    Reference negatedTwice = walker.referenceOf(index.value(inner).valueOperand());
    TypeID type = negatedTwice && negatedTwice.isRecord() ? walker.typeOf(negatedTwice) : TypeID();
    if (!type || !walker.types().at(type).isBoolean())
        return false;
    walker.bind(negation.id(), negatedTwice);
    return true;
}

// ---- Identities

// `x & 0`, `x % 1` of what is never negative: the ranges of the operands leave one integer. Undefined, null, booleans and numbers convert without running code, and none of them throws beside another.
static Reference constantWhereRangesLeaveOneInteger(Walker& walker, Opcode opcode, const OperandsInNumberOperators& operands)
{
    if (numberOfNamedOperandsOf(opcode) != 2 || (!operands[0].known && !operands[1].known))
        return { };
    if (!operands[0].type.isNumberLike() || !operands[1].type.isNumberLike())
        return { };
    OperatorTypes types { walker.typeOf(operands[0].reference), walker.typeOf(operands[1].reference) };
    Type result = walker.types().at(chooseOperatorForm(walker.program(), walker.types(), opcode, types).result);
    return result.isInteger() && result.integers.isSingleValue() ? integerInNumberForms(walker, result.integers.minimum) : Reference();
}

// `(s + 8) - s`. s is one value, so an int32 here was an int32 at the Add: the Add was numeric, and with |c| < 2^31 it was exact.
static std::optional<int64_t> constantAddedToSubtrahend(Walker& walker, const OperandsInNumberOperators& operands)
{
    if (!operands[1].type.isInt32())
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID sum = index.valueBehindCasts(operands[0].written);
    if (!sum || index.isParameter(sum) || index.opcodeOf(sum) != Opcode::Add)
        return std::nullopt;
    BodyValue addition = index.value(sum);
    if (index.valueBehindCasts(addition.operand(0)) != index.valueBehindCasts(operands[1].written))
        return std::nullopt;
    std::optional<int64_t> addend = integerKnownInNumberForms(walker, walker.referenceOf(addition.operand(1)));
    if (!addend || std::abs(*addend) > std::numeric_limits<int32_t>::max())
        return std::nullopt;
    return addend;
}

// An older value or a constant that is what the operator gives, -0 and NaN among its operands too. Null: there is none.
static Reference valueThatArithmeticGives(Walker& walker, Opcode opcode, const OperandsInNumberOperators& operands)
{
    const OperandInNumberOperators& left = operands[0];
    const OperandInNumberOperators& right = operands[1];
    switch (opcode) {
    case Opcode::Add:
        for (uint32_t side = 0; side < 2; ++side) {
            const OperandInNumberOperators& other = operands[1 - side];
            // -0 + 0 is 0, and -0 + -0 is -0.
            if (isKnownIntegerInNumberOperators(operands[side].known, 0) && other.type.isInteger())
                return other.reference;
            if (isNegativeZeroInNumberOperators(operands[side].known) && other.type.isNumber())
                return other.reference;
        }
        return { };
    case Opcode::Sub:
        // -0 - 0 is -0, and -0 - -0 is 0.
        if (isKnownIntegerInNumberOperators(right.known, 0) && left.type.isNumber())
            return left.reference;
        if (isNegativeZeroInNumberOperators(right.known) && left.type.isInteger())
            return left.reference;
        if (std::optional<int64_t> addend = constantAddedToSubtrahend(walker, operands))
            return integerInNumberForms(walker, *addend);
        return { };
    case Opcode::Mul:
        for (uint32_t side = 0; side < 2; ++side) {
            if (isKnownIntegerInNumberOperators(operands[side].known, 1) && operands[1 - side].type.isNumber())
                return operands[1 - side].reference;
        }
        return { };
    case Opcode::Div:
        return isKnownIntegerInNumberOperators(right.known, 1) && left.type.isNumber() ? left.reference : Reference();
    case Opcode::Pow:
        if (!left.type.isNumber())
            return { };
        // NaN ** 0 is 1.
        if (isKnownIntegerInNumberOperators(right.known, 0) || isNegativeZeroInNumberOperators(right.known))
            return integerInNumberForms(walker, 1);
        return isKnownIntegerInNumberOperators(right.known, 1) ? left.reference : Reference();
    default:
        return { };
    }
}

// ---- Bit operators

static ValueID recordBehindNamesInNumberOperators(const Code& code, ValueID record)
{
    while (opcodesThatGiveSameValue.contains(code.at(record).opcode))
        record = code.at(record).operand(0);
    return record;
}

// `number` is what Math.trunc() gave a proven number: that number, as the body writes it. Math.floor is not looked through: it differs from truncation for negative x.
static ValueID numberThatWasTruncatedInNumberOperators(Walker& walker, ValueID number)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = index.valueBehindCasts(number);
    if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::Call)
        return { };
    Reference built = walker.referenceOf(written);
    if (!built.isValue())
        return { };
    const Operation& record = walker.code().at(recordBehindNamesInNumberOperators(walker.code(), built.id()));
    if (record.opcode != Opcode::CallInline || record.inlineBuiltin() != InlineBuiltin::MathTrunc || record.numberOfVariadicOperands() != 1)
        return { };
    auto arguments = index.value(written).as<Opcode::Call>().argumentsOperands();
    if (!arguments.size())
        return { };
    ValueID truncated = *arguments.begin();
    Reference taken = walker.referenceOf(truncated);
    // `Math.trunc.call(7, x)` writes 7 first and truncates x.
    if (!taken || !taken.isValue() || recordBehindNamesInNumberOperators(walker.code(), taken.id()) != recordBehindNamesInNumberOperators(walker.code(), record.variadicOperands()[0]))
        return { };
    TypeID type = walker.typeOf(taken);
    return type && walker.types().at(type).isNumber() ? truncated : ValueID();
}

// `(a / b) | 0`, `(i % n) | 0`, `Math.trunc(x) | 0`. ToInt32 truncates by itself, and gives 0 for NaN, -0 and the infinities whether or not they were truncated first.
// Sums and products are not looked through: beyond 2^53 the number is rounded, so its low bits are not the ones that an integer register would hold.
// Each of the seven operators looks only at the low 32 bits of the integer part of an operand that is a number, so any other number with the same bits may stand for it, whatever the other operand is.
static Reference buildSimplerOperandOfBitOperator(Walker& walker, Origin origin, ValueID written)
{
    InputsOfTypedOperation inputs(walker, nullptr);
    ValueID number = written;
    while (true) {
        if (std::optional<DivisionAsWritten> quotient = findQuotientOfIntegers(walker, number, inputs))
            return buildDivisionOfIntegers(walker, origin, walker.referenceOf(quotient->dividend), walker.referenceOf(quotient->divisor), QuotientRounding::ToInt32);
        if (std::optional<WholeRemainder> remainder = findWholeRemainder(walker, number, inputs))
            return buildWholeRemainder(walker, origin, *remainder);
        ValueID truncated = numberThatWasTruncatedInNumberOperators(walker, number);
        if (!truncated)
            break;
        number = truncated;
    }
    return number == written ? Reference() : walker.referenceOf(number);
}

// With these bits in the operand at that index, the operator gives ToInt32 of its other operand, and URShift gives ToUint32.
static bool isIdentityElementOfBitOperator(Opcode opcode, uint32_t operandIndex, int32_t bits)
{
    switch (opcode) {
    case Opcode::BitOr:
    case Opcode::BitXor:
        return !bits;
    case Opcode::BitAnd:
        return bits == -1;
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
        return operandIndex == 1 && !(bits & 31);
    default:
        return false;
    }
}

static Reference buildConversionInNumberOperators(Walker& walker, Opcode conversion, Reference number, bool givesOnlyLowestBits, Origin origin)
{
    Draft draft(conversion, origin);
    walker.chooseConservativeFields(draft);
    draft.setValueOperand(number);
    if (givesOnlyLowestBits)
        draft.setGivesOnlyLowestBits(true);
    return walker.build(WTF::move(draft));
}

// `x | 0`, `x >>> 0`, `~~x` of a proven number. An integer type holds no -0, so an integer that the conversion leaves as it is stands for it.
static Reference buildBitsOfNumberInNumberOperators(Walker& walker, Opcode conversion, Reference number, Origin origin)
{
    Type type = walker.types().at(walker.typeOf(number));
    Range leftAsTheyAre = conversion == Opcode::ToUint32 ? Range::uint32() : Range::int32();
    if (type.isInteger() && type.integers.isSubsetOf(leftAsTheyAre) && walker.has(Mechanism::Folding))
        return number;
    // The bits are those of ToInt32, which the other operators ask for.
    if (conversion == Opcode::ToUint32 && !type.isInteger()) {
        number = buildConversionInNumberOperators(walker, Opcode::ToInt32, number, false, origin);
        if (!number)
            return { };
    }
    return buildConversionInNumberOperators(walker, conversion, number, false, origin);
}

// `(x | 0) + (x >> 3)` in a loop. The code of a conversion branches to a call for what lies beyond int64. As an operation of its own it leaves the loop where the number stands in front, and is found again there.
// An integer stays as it is: the operator takes the low half of its register. False: nothing was converted.
static bool convertOperandsOfBitOperatorOnce(Walker& walker, Opcode opcode, Origin origin, std::span<Reference> operands)
{
    if (!walker.isInLoop())
        return false;
    TypeArena& types = walker.types();
    for (Reference operand : operands) {
        if (!types.at(walker.typeOf(operand)).isNumberLike())
            return false;
    }

    bool hasConverted = false;
    for (uint32_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex) {
        Reference number = operands[operandIndex];
        Type type = types.at(walker.typeOf(number));
        if (!(type.primitives & Primitive::Double))
            continue;
        hasConverted = true;
        if (!type.isNumber())
            number = buildOperatorInNumberForms(walker, Opcode::ToNumber, number, { }, origin);
        if (!number) {
            operands[operandIndex] = { };
            continue;
        }
        // The tag of a boxed number is looked at in any case, and an int32 behind it has every bit.
        bool isMasked = opcode == Opcode::BitAnd && isMaskOfLowestBitsOfInt32(types.at(walker.typeOf(operands[1 - operandIndex]))) && walker.representationOf(number) != Representation::JSValue;
        operands[operandIndex] = buildConversionInNumberOperators(walker, Opcode::ToInt32, number, isMasked, origin);
    }
    return hasConverted;
}

static bool buildBitOperator(Walker& walker, const BodyValue& value, OperandsInNumberOperators& operands)
{
    Opcode opcode = value.opcode();
    uint32_t numberOfOperands = numberOfNamedOperandsOf(opcode);
    Origin origin = originInFrontOfNumberOperator(walker, value);

    bool takesOtherOperands = false;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        OperandInNumberOperators& operand = operands[operandIndex];
        Reference simpler = buildSimplerOperandOfBitOperator(walker, origin, operand.written);
        if (!simpler)
            simpler = integerThatCharCodeIsKnownToBe(walker, value, operand.written);
        if (walker.isUnreachable())
            return true;
        if (!simpler)
            continue;
        takesOtherOperands = true;
        operand.reference = simpler;
        operand.type = walker.types().at(walker.typeOf(simpler));
        operand.known = primitiveKnownInNumberForms(walker, simpler);
    }

    if (walker.has(Mechanism::Folding)) {
        if (Reference constant = constantWhereRangesLeaveOneInteger(walker, opcode, operands))
            return bindInNumberOperators(walker, value, constant);
    }

    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands && numberOfOperands == 2; ++operandIndex) {
        const OperandInNumberOperators& other = operands[1 - operandIndex];
        PrimitiveKnownToFolding known = operands[operandIndex].known;
        if (!other.type.isNumber() || known.kind() != PrimitiveKnownToFolding::Kind::Number)
            continue;
        if (isIdentityElementOfBitOperator(opcode, operandIndex, toInt32(known.asNumber())))
            return bindInNumberOperators(walker, value, buildBitsOfNumberInNumberOperators(walker, opcode == Opcode::URShift ? Opcode::ToUint32 : Opcode::ToInt32, other.reference, origin));
    }

    // `~~x`
    if (opcode == Opcode::BitNot && !takesOtherOperands) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID inner = index.valueBehindCasts(operands[0].written);
        if (inner && !index.isParameter(inner) && index.opcodeOf(inner) == Opcode::BitNot) {
            Reference inverted = walker.referenceOf(index.value(inner).valueOperand());
            TypeID type = inverted && inverted.isRecord() ? walker.typeOf(inverted) : TypeID();
            if (type && walker.types().at(type).isNumber())
                return bindInNumberOperators(walker, value, buildBitsOfNumberInNumberOperators(walker, Opcode::ToInt32, inverted, origin));
        }
    }

    std::array<Reference, 2> taken { operands[0].reference, operands[1].reference };
    std::span<Reference> takenByOperator = std::span(taken).first(numberOfOperands);
    takesOtherOperands |= convertOperandsOfBitOperatorOnce(walker, opcode, origin, takenByOperator);
    if (!takesOtherOperands)
        return false;
    return buildOnOperandsInNumberOperators(walker, value, takenByOperator);
}

// ---- Division behind a test of the remainder

static constexpr OpcodeSet opcodesBetweenRemainderAndBranchInNumberOperators { Opcode::StrictEq, Opcode::Eq, Opcode::ToBoolean, Opcode::LogicalNot };

// The Mod that the Branch has found to be zero, or NaN, on that edge. Null: it asks nothing of the kind.
static ValueID remainderFoundZeroOnEdgeInNumberOperators(const BodyIndex& index, const BodyTerminator& branch, uint32_t targetIndex)
{
    if (!branch.isConditionalBranch())
        return { };
    bool holds = !targetIndex;
    ValueID condition = index.valueBehindCasts(branch.operand(0));
    while (condition && !index.isParameter(condition) && index.opcodeOf(condition) == Opcode::LogicalNot) {
        holds = !holds;
        condition = index.valueBehindCasts(index.value(condition).valueOperand());
    }
    if (!condition || index.isParameter(condition))
        return { };

    BodyValue test = index.value(condition);
    ValueID tested;
    if (test.opcode() == Opcode::ToBoolean && !holds)
        tested = test.valueOperand();
    else if ((test.opcode() == Opcode::StrictEq || test.opcode() == Opcode::Eq) && holds) {
        for (uint32_t side = 0; side < 2; ++side) {
            ValueID other = test.operand(1 - side);
            if (other && !index.isParameter(other) && index.opcodeOf(other) == Opcode::Integer && !index.value(other).as<Opcode::Integer>().integer())
                tested = test.operand(side);
        }
    }
    tested = tested ? index.valueBehindCasts(tested) : ValueID();
    return tested && !index.isParameter(tested) && index.opcodeOf(tested) == Opcode::Mod ? tested : ValueID();
}

// `if (a % b === 0) q = a / b`. By the body, in a number of steps that does not grow with it: the one edge into the block of the Div has found the remainder of the same two values zero.
// Or the first mention of the dividend is that Mod, nothing but negations stands between its test and a Branch, and the edge on which it is zero alone enters a block that dominates the Div.
static bool standsBehindTestThatRemainderIsZero(Walker& walker, const BodyValue& quotient)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID dividend = index.valueBehindCasts(quotient.operand(0));
    ValueID divisor = index.valueBehindCasts(quotient.operand(1));
    auto dividesTheSame = [&](ValueID remainder) {
        if (!remainder)
            return false;
        BodyValue written = index.value(remainder);
        return index.valueBehindCasts(written.operand(0)) == dividend && index.valueBehindCasts(written.operand(1)) == divisor;
    };

    BlockID block = index.blockOf(quotient.id());
    if (std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(block)) {
        if (dividesTheSame(remainderFoundZeroOnEdgeInNumberOperators(index, index.terminator(edge->from), edge->targetIndex)))
            return true;
    }

    std::optional<BodyUser> firstMention = index.firstUser(dividend);
    if (!firstMention || firstMention->kind != BodyUseKind::Operand || index.opcodeOf(firstMention->value) != Opcode::Mod || !dividesTheSame(firstMention->value))
        return false;
    ValueID remainder = firstMention->value;
    std::optional<BodyUser> user = index.firstUser(remainder);
    while (user && user->kind == BodyUseKind::Operand && opcodesBetweenRemainderAndBranchInNumberOperators.contains(index.opcodeOf(user->value)))
        user = index.onlyUser(user->value);
    if (!user || user->kind != BodyUseKind::TerminatorOperand)
        return false;

    BodyTerminator branch = index.terminator(user->block);
    if (!branch.isConditionalBranch())
        return false;
    uint32_t targetIndex = 0;
    for (BodyTarget target : branch.targets()) {
        if (remainderFoundZeroOnEdgeInNumberOperators(index, branch, targetIndex++) != remainder)
            continue;
        BlockID whereZero = target.block();
        return index.numberOfIncomingEdges(whereZero) == 1 && !index.isEnteredByHandlerEdge(whereZero) && index.dominates(whereZero, block);
    }
    return false;
}

// Two integers that leave no remainder have an integer for their quotient, no further from zero than the dividend. A divisor above zero makes no -0 of a dividend that is zero.
static bool buildDivisionWithoutRemainder(Walker& walker, const BodyValue& quotient, const OperandsInNumberOperators& operands)
{
    if (!operands[0].type.isInt32() || !operands[1].type.isInt32() || operands[1].type.integers.minimum <= 0)
        return false;
    if (!standsBehindTestThatRemainderIsZero(walker, quotient))
        return false;
    return bindInNumberOperators(walker, quotient, buildDivisionOfIntegers(walker, walker.originOf(quotient), operands[0].reference, operands[1].reference, QuotientRounding::ToInt32));
}

// ---- Whole remainders that are compared

// `x % 2 === 0`, `a % n === b % n`, `i % n < 2`. -0 equals and compares as 0 does, so a remainder of integers is compared as the integer that it is. NaN equals nothing, and makes every relation false.
static bool buildComparisonOfWholeRemainders(Walker& walker, const BodyValue& comparison, const OperandsInNumberOperators& operands)
{
    InputsOfTypedOperation inputs(walker, nullptr);
    std::array<std::optional<WholeRemainder>, 2> remainders { findWholeRemainder(walker, operands[0].written, inputs), findWholeRemainder(walker, operands[1].written, inputs) };
    if (!remainders[0] && !remainders[1])
        return false;
    for (uint32_t index = 0; index < 2; ++index) {
        if (!remainders[index] && !operands[index].type.isNumber())
            return false;
    }

    bool isEquality = allEqualities.contains(comparison.opcode());
    Origin origin = originInFrontOfNumberOperator(walker, comparison);
    std::array<Reference, 2> compared { operands[0].reference, operands[1].reference };
    Vector<ValueID, 2> divisorsThatMayBeZero;
    for (uint32_t index = 0; index < 2; ++index) {
        if (!remainders[index])
            continue;
        const OperandInNumberOperators& other = operands[1 - index];
        std::optional<int64_t> knownOther = isEquality && !remainders[1 - index] ? integerKnownInNumberForms(walker, other.reference) : std::nullopt;
        if (knownOther) {
            if (std::optional<BitsOfDividendCompared> bits = tryCompareBitsOfDividend(walker, origin, *remainders[index], *knownOther)) {
                compared[index] = bits->bits;
                compared[1 - index] = bits->comparedWith;
                continue;
            }
        }
        compared[index] = buildWholeRemainder(walker, origin, *remainders[index]);
        // In place of NaN stands 0, which differs from whatever is never zero as NaN does.
        bool differsFromZeroAnyway = isEquality && other.type.isInteger() && !other.type.integers.contains(0);
        if (remainders[index]->mayBeNaN && !differsFromZeroAnyway)
            divisorsThatMayBeZero.append(remainders[index]->divisor);
    }

    Draft draft = walker.draftOf(comparison);
    draft.setOperand(0, compared[0]);
    draft.setOperand(1, compared[1]);
    return bindInNumberOperators(walker, comparison, buildComparisonWhereDivisorsAreNotZero(walker, WTF::move(draft), divisorsThatMayBeZero.span()));
}

// ---- Loose equality with a text that is no number

// `anything == "abc"`. No number, boolean or BigInt equals a text that converts to NaN, so among primitives only that text does: one call that converts nothing for primitives.
// Where something is expected of the value a guess gives more: it is asked behind this link, so the operation is left to it, and comes here in its closed arm.
static bool buildLooseEqualityWithTextThatIsNoNumber(Walker& walker, const BodyValue& equality, const OperandsInNumberOperators& operands)
{
    if (equality.opcode() != Opcode::Eq || operandsNarrowedBehindTestInNumberForms(walker, equality.id()) != std::optional<uint32_t>(0))
        return false;
    for (uint32_t textIndex = 0; textIndex < 2; ++textIndex) {
        const OperandInNumberOperators& text = operands[textIndex];
        const OperandInNumberOperators& subject = operands[1 - textIndex];
        if (!text.known.isText() || liesInThread(text.known.asText()))
            continue;
        if (!subject.type.hasObjects() || !subject.type.isJavaScriptValue() || subject.type.mayBeUnspecified() || subject.type.primitives & Primitive::Absent)
            continue;
        Vector<char16_t> buffer;
        if (!std::isnan(jsToNumber(walker.types().viewOf(text.known.asText(), buffer))))
            continue;
        if (walker.has(Mechanism::Guesses) && walker.opensGuesses() && findExpectation(walker, subject.written).type)
            return false;

        Draft written = walker.draftOf(equality);
        Draft draft(Opcode::ValueLooselyEqualsLiteral, written.origin());
        walker.chooseConservativeFields(draft);
        draft.setValueOperand(subject.reference);
        draft.setLiteral(text.known.asText());
        draft.expected = written.expected;
        draft.inputValueGiven = equality.id();
        return bindInNumberOperators(walker, equality, walker.build(WTF::move(draft)));
    }
    return false;
}

// ---- An integer against a fraction

static bool relationAsksForLesserOnLeft(Opcode relation)
{
    return relation == Opcode::Less || relation == Opcode::LessEq;
}

static Opcode relationWithSidesSwapped(Opcode relation)
{
    switch (relation) {
    case Opcode::Less:
        return Opcode::Greater;
    case Opcode::LessEq:
        return Opcode::GreaterEq;
    case Opcode::Greater:
        return Opcode::Less;
    case Opcode::GreaterEq:
        return Opcode::LessEq;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// `relationOfInteger` is read with the integer on the left. i < 2.5 is i < 3 and i >= 2.5 is i >= 3. i <= 2.5 is i <= 2 and i > 2.5 is i > 2.
static IntegerInPlaceOfNumber integerInPlaceOfNumber(Opcode relationOfInteger, double number)
{
    if (std::isnan(number))
        return { false, 0 };
    bool roundsUp = relationOfInteger == Opcode::Less || relationOfInteger == Opcode::GreaterEq;
    double rounded = roundsUp ? std::ceil(number) : std::floor(number);
    static_assert(static_cast<double>(maxTrackedInteger + 1) == 0x1p62);
    if (rounded >= 0x1p62)
        return { relationAsksForLesserOnLeft(relationOfInteger), 0 };
    if (rounded <= -0x1p62)
        return { !relationAsksForLesserOnLeft(relationOfInteger), 0 };
    return { std::nullopt, static_cast<int64_t>(rounded) };
}

// The comparison answers as before, so what an edge behind it proves of the integer holds, and now its range is cut as well. It keeps its opcode and the order of its operands.
static bool buildRelationWithIntegerInPlaceOfNumber(Walker& walker, const BodyValue& relation, const OperandsInNumberOperators& operands)
{
    for (uint32_t numberIndex = 0; numberIndex < 2; ++numberIndex) {
        PrimitiveKnownToFolding number = operands[numberIndex].known;
        if (number.kind() != PrimitiveKnownToFolding::Kind::Number || isTrackedInteger(number.asNumber()) || !operands[1 - numberIndex].type.isInteger())
            continue;
        Opcode relationOfInteger = numberIndex ? relation.opcode() : relationWithSidesSwapped(relation.opcode());
        IntegerInPlaceOfNumber integer = integerInPlaceOfNumber(relationOfInteger, number.asNumber());
        if (integer.answer)
            return bindInNumberOperators(walker, relation, booleanInNumberForms(walker, *integer.answer));
        std::array<Reference, 2> compared { operands[0].reference, operands[1].reference };
        compared[numberIndex] = integerInNumberForms(walker, integer.bound);
        return buildOnOperandsInNumberOperators(walker, relation, compared);
    }
    return false;
}

// The record is older than the parameters of the head of the innermost loop of the body around `user`, or lies in a block that is.
static bool standsInFrontOfLoopAroundInNumberOperators(Walker& walker, const BodyValue& user, Reference reference)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(user.id()));
    if (!loop || !reference.isValue())
        return false;
    BodyBlock head = index.block(index.loop(loop).head);
    if (!head.numberOfParameters())
        return false;
    Reference first = walker.referenceBoundTo(head.parameter(0));
    if (!first.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& parameter = code.at(first.id());
    if (parameter.opcode != Opcode::Parameter || !parameter.block() || !code.at(parameter.block()).isLoopHead)
        return false;
    if (reference.id().index() < first.id().index())
        return true;
    BlockID block = code.blockOf(reference.id());
    return block && block.index() < parameter.block().index();
}

// `for (..; i < n / 2; ..)` in a loop that leaves n alone. The number is rounded once, in front of the loop, and each turn compares integers.
// A bound that has to stand in the loop costs more than the conversion of the integer that it saves, so nobody uses it.
static bool buildRelationWithNumberRoundedInFrontOfLoop(Walker& walker, const BodyValue& relation, const OperandsInNumberOperators& operands)
{
    if (!walker.isInLoop() || !walker.has(Mechanism::LoopInvariants))
        return false;
    for (uint32_t numberIndex = 0; numberIndex < 2; ++numberIndex) {
        const OperandInNumberOperators& number = operands[numberIndex];
        if (!operands[1 - numberIndex].type.isInteger() || !number.type.isNumber() || number.type.isInteger())
            continue;
        // The integer is held as a double, so the two are compared as doubles. A number that is held boxed is unboxed once, in front of the loop: the relation would look at its tag in every turn.
        if (operands[1 - numberIndex].reference.isValue() && walker.representationOf(operands[1 - numberIndex].reference) == Representation::Double) {
            Representation held = walker.representationOf(number.reference);
            if (held == Representation::Double || !standsInFrontOfLoopAroundInNumberOperators(walker, relation, number.reference))
                return false;
            std::optional<ConversionPath> path = findConversion(walker.types(), walker.typeOf(number.reference), held, Representation::Double);
            if (!path)
                return false;
            Reference unboxed = number.reference;
            for (ConversionStep step : *path) {
                Draft conversion(step.opcode, originInFrontOfNumberOperator(walker, relation));
                conversion.setOperand(0, unboxed);
                conversion.representation = step.to;
                unboxed = walker.build(WTF::move(conversion));
                if (!unboxed)
                    return true;
            }
            std::array<Reference, 2> compared { operands[0].reference, operands[1].reference };
            compared[numberIndex] = unboxed;
            return buildOnOperandsInNumberOperators(walker, relation, compared);
        }
        if (!standsInFrontOfLoopAroundInNumberOperators(walker, relation, number.reference))
            return false;

        Draft bound(Opcode::IntegerBound, originInFrontOfNumberOperator(walker, relation));
        walker.chooseConservativeFields(bound);
        bound.setNumberOperand(number.reference);
        bound.setComparison(relation.opcode());
        bound.setNumberIsOnLeft(!numberIndex);
        Reference rounded = walker.build(WTF::move(bound));
        if (!rounded)
            return true;
        std::array<Reference, 2> compared { operands[0].reference, operands[1].reference };
        if (walker.code().blockOf(rounded.id()) != walker.currentBlock())
            compared[numberIndex] = rounded;
        return buildOnOperandsInNumberOperators(walker, relation, compared);
    }
    return false;
}

// ---- Number-likes in front of a relation

// `n < done`, `count <= undefined`. ToNumber of a boolean, undefined or null runs no code, and the relation on what it gives has the form Numbers or Integers.
// What an edge behind a test narrows is an operand of the test, so an operand that an edge hands on stays as it is.
static bool buildRelationOnNumberLikes(Walker& walker, const BodyValue& relation, const InputsOfTypedOperation& inputs)
{
    std::array<bool, 2> isConverted { };
    for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
        TypeID id = inputs.typeOf(relation.operand(operandIndex));
        if (!id || isNeverType(id))
            return false;
        Type type = walker.types().at(id);
        if (!type.isNumberLike())
            return false;
        isConverted[operandIndex] = !isTakenAsItIsByFormNumbers(type);
    }
    if (!isConverted[0] && !isConverted[1])
        return false;
    std::optional<uint32_t> narrowed = operandsNarrowedBehindTestInNumberForms(walker, relation.id());
    if (!narrowed || (isConverted[0] && *narrowed & 1) || (isConverted[1] && *narrowed & 2))
        return false;
    if (inputs.areOnlyTypes())
        return true;

    Origin origin = originInFrontOfNumberOperator(walker, relation);
    std::array<Reference, 2> compared;
    for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
        compared[operandIndex] = walker.referenceOf(relation.operand(operandIndex));
        if (isConverted[operandIndex])
            compared[operandIndex] = buildOperatorInNumberForms(walker, Opcode::ToNumber, compared[operandIndex], { }, origin);
    }
    return buildOnOperandsInNumberOperators(walker, relation, compared);
}

// ---- Sums

// -0 + i and i - -0 are i for every integer i, which is never -0. A char code behind a test that NaN fails is its code unit.
static bool buildSumOnIntegersInPlaceOfNumbers(Walker& walker, const BodyValue& sum, const OperandsInNumberOperators& operands)
{
    Opcode opcode = sum.opcode();
    uint32_t numberOfOperands = numberOfNamedOperandsOf(opcode);
    InputsOfTypedOperation inputs(walker, nullptr);
    Origin origin = originInFrontOfNumberOperator(walker, sum);

    std::array<Reference, 2> taken { operands[0].reference, operands[1].reference };
    bool takesOtherOperands = false;
    for (uint32_t side = 0; side < numberOfOperands; ++side) {
        if (Reference codeUnit = integerThatCharCodeIsKnownToBe(walker, sum, operands[side].written)) {
            taken[side] = codeUnit;
            takesOtherOperands = true;
            continue;
        }
        bool zeroesAreAlike = (opcode == Opcode::Add || (opcode == Opcode::Sub && side == 1)) && operands[1 - side].type.isInteger();
        std::optional<WholeRemainder> remainder = zeroesAreAlike ? findWholeRemainder(walker, operands[side].written, inputs) : std::nullopt;
        if (!remainder || remainder->mayBeNaN)
            continue;
        taken[side] = buildWholeRemainder(walker, origin, *remainder);
        takesOtherOperands = true;
    }
    return takesOtherOperands && buildOnOperandsInNumberOperators(walker, sum, std::span<const Reference>(taken).first(numberOfOperands));
}

// `sum += a[i++]` in a loop. Where the number is read once more behind its step, the old and the new number need a register each, and the loop moves the new one back into place.
// Behind the last read the old number dies at the step, which then adds in place. The step is pure, and the one operation that it waits for does not name it.
static bool buildStepBehindLastReadOfItsNumber(Walker& walker, const BodyValue& step, const OperandsInNumberOperators& operands)
{
    if (!walker.isInLoop())
        return false;
    Opcode opcode = step.opcode();
    uint32_t steppedOperand = 0;
    if (opcode != Opcode::Increment && opcode != Opcode::Decrement) {
        if (opcode != Opcode::Add && opcode != Opcode::Sub)
            return false;
        if (opcode == Opcode::Add && operands[0].reference.isConstant())
            steppedOperand = 1;
        if (!operands[1 - steppedOperand].reference.isConstant())
            return false;
    }

    const OperandInNumberOperators& stepped = operands[steppedOperand];
    Representation form = walker.representationOf(stepped.reference);
    if (!stepped.reference.isValue() || !integerRepresentations.contains(form))
        return false;
    // A sum that keeps the form of the number has not left it.
    Draft asWritten = walker.draftOf(step);
    walker.chooseConservativeFields(asWritten);
    if (formAllowedForTypedOperator(walker, asWritten) != OperatorForm::Integers)
        return false;
    asWritten.setForm(OperatorForm::Integers);
    if (resultRepresentationOf(walker, asWritten, walker.resultTypeOf(asWritten)) != form)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(step.id()));
    uint32_t placeOfReader = step.id().index() + 1 - block.firstValue().index();
    if (index.hasOneUse(stepped.written) || placeOfReader >= walker.endOfStretchBeingWalked() || placeOfReader >= block.numberOfValues())
        return false;
    // Where the read throws, a handler may read the new number.
    if (block.hasHandler())
        return false;
    BodyValue reader = index.value(ValueID(step.id().index() + 1));
    if (opcodesThatStateTypes.contains(reader.opcode()))
        return false;
    bool namesStep = false;
    reader.forEachOperand([&](ValueID operand, uint32_t) {
        namesStep |= operand == step.id();
    });
    if (namesStep)
        return false;
    // What is found of a body is kept with its index, so whoever asks later reads that.
    BodyLiveness liveness;
    liveness.compute(index);
    if (liveness.lastUse(stepped.written) != reader.id())
        return false;
    // Walked by itself, the read opens no guess that reaches behind it. So it is one that needs none.
    auto typeOfInput = [&](ValueID input) {
        return walker.typeOf(walker.referenceOf(input));
    };
    TypedForm formOfReader = typedFormOf(walker, reader, TypesOfInputs(typeOfInput));
    walker.takeFormAskedAhead();
    if (formOfReader != TypedForm::Built)
        return false;

    walker.walkStretch(block.id(), placeOfReader, placeOfReader + 1);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    return bindInNumberOperators(walker, step, buildInAllowedFormInNumberForms(walker, walker.draftOf(step)));
}

// ---- The link

static_assert(opcodesThatBuildNumberFormTakes == (opcodesOfNumberOperators));

bool buildNumberFormOfItsOpcodes(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfNumberOperators.contains(opcode) || !hasMechanismsOfNumberForms(walker))
        return false;
    if (inputs.areOnlyTypes())
        return allRelations.contains(opcode) && buildRelationOnNumberLikes(walker, value, inputs);

    if (opcode == Opcode::DirectEval)
        return bindWhatDirectEvalHandsBack(walker, value);
    if (opcode == Opcode::LogicalNot)
        return bindWhatIsNegatedTwice(walker, value);

    // Each form of a comparison and of a truth leans on a number, a text or a constant in sight.
    if (allEqualities.contains(opcode) || allRelations.contains(opcode) || opcode == Opcode::ToBoolean) {
        bool somethingIsProven = false;
        for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(opcode); ++operandIndex)
            somethingIsProven |= !walker.nothingIsProvenOf(walker.referenceOf(value.operand(operandIndex)));
        if (!somethingIsProven)
            return false;
    }

    OperandsInNumberOperators operands;
    if (!readOperandsInNumberOperators(walker, value, operands))
        return false;

    if (arithmeticInNumberOperators.contains(opcode) && walker.has(Mechanism::Folding)) {
        if (Reference given = valueThatArithmeticGives(walker, opcode, operands))
            return bindInNumberOperators(walker, value, given);
        if (Reference constant = constantWhereRangesLeaveOneInteger(walker, opcode, operands))
            return bindInNumberOperators(walker, value, constant);
    }
    // x ** 2 is x * x.
    if (opcode == Opcode::Pow && operands[0].type.isNumber() && isKnownIntegerInNumberOperators(operands[1].known, 2))
        return bindInNumberOperators(walker, value, buildOperatorInNumberForms(walker, Opcode::Mul, operands[0].reference, operands[0].reference, walker.originOf(value)));

    if (allBitwiseOperators.contains(opcode))
        return buildBitOperator(walker, value, operands);
    if (opcode == Opcode::Div)
        return buildDivisionWithoutRemainder(walker, value, operands);
    if (opcode == Opcode::Mod)
        return buildNumberRemainderWithItsTest(walker, value);

    if (allEqualities.contains(opcode)) {
        if (buildComparisonOfWholeRemainders(walker, value, operands))
            return true;
        if (buildComparisonByCodeUnit(walker, value, inputs))
            return true;
#if __has_include("AOTWalkNumberSlots.h")
        if (buildComparisonWithWrittenText(walker, value, inputs))
            return true;
#endif
        return buildLooseEqualityWithTextThatIsNoNumber(walker, value, operands);
    }

    if (allRelations.contains(opcode)) {
        if (buildRelationWithIntegerInPlaceOfNumber(walker, value, operands))
            return true;
        if (buildRelationWithNumberRoundedInFrontOfLoop(walker, value, operands))
            return true;
        if (buildComparisonOfWholeRemainders(walker, value, operands))
            return true;
        if (buildComparisonByCodeUnit(walker, value, inputs))
            return true;
        return buildRelationOnNumberLikes(walker, value, inputs);
    }

    // `if (i % 2)`. 0, -0 and NaN are falsy alike.
    if (opcode == Opcode::ToBoolean) {
        std::optional<WholeRemainder> remainder = findWholeRemainder(walker, operands[0].written, inputs);
        if (!remainder)
            return false;
        Reference integer = buildZeroWhereWholeRemainderIsZero(walker, originInFrontOfNumberOperator(walker, value), *remainder);
        return buildOnOperandsInNumberOperators(walker, value, singleElementSpan(integer));
    }

    if (buildSumOnIntegersInPlaceOfNumbers(walker, value, operands))
        return true;
    return buildStepBehindLastReadOfItsNumber(walker, value, operands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
