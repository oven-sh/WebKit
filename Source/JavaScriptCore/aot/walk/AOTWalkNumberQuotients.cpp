/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations with UnboxedNumbers: what `/` and `%` make of two proven integers is divided as integers wherever whoever reads it cannot tell. Who reads is the asker's to know.
// It goes by the body as written behind an operand and by the proven types, and keeps nothing. What it supersedes stays built, and the sweep drops it.
// At a suspension and at a throw: nothing of its own, each operation is pure. Withheld: nobody asks.

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include <array>
#include <cstdlib>
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!OperandIndex::Div::left && OperandIndex::Div::right == 1 && !OperandIndex::Mod::left && OperandIndex::Mod::right == 1);
static_assert(!OperandIndex::DivideIntegers::dividend && OperandIndex::DivideIntegers::divisor == 1);
static_assert(!OperandIndex::RemainderOfIntegers::dividend && OperandIndex::RemainderOfIntegers::divisor == 1);

static Type typeOfBuiltInQuotients(Walker& walker, Reference reference)
{
    TypeID id = walker.typeOf(reference);
    return walker.types().at(id ? id : TypeID(TypeTable::never));
}

// DivideIntegers and RemainderOfIntegers have code for no other integers.
static bool isExactIntegerInQuotients(Walker& walker, TypeID id)
{
    if (!id)
        return false;
    Type type = walker.types().at(id);
    return type.isInteger() && type.integers.isSubsetOf(Range::exact());
}

static ValueID recordBehindNamesAndFormsInQuotients(const Code& code, ValueID record)
{
    while (opcodesThatGiveSameCell.contains(code.at(record).opcode))
        record = code.at(record).operand(0);
    return record;
}

// In the body an operand of a definition dominates every user of that definition. In Code it need not: the rejoin of a guess hands over what is mentioned behind the stretch and nothing else,
// so `list[i % list.length]` may find a parameter for the remainder while the length is still bound to a record of the typed arm.
// Where the record that stands for the operator is the operator itself, on the records that stand for its operands, they dominate it, and it dominates the place where the walk stands.
static bool operandsAreInSightInQuotients(Walker& walker, const BodyValue& operation)
{
    const Code& code = walker.code();
    Reference built = walker.referenceBoundTo(operation.id());
    if (!built || !built.isValue())
        return false;
    const Operation& record = code.at(recordBehindNamesAndFormsInQuotients(code, built.id()));
    if (record.opcode != operation.opcode())
        return false;
    for (uint32_t operandIndex : { 0u, 1u }) {
        Reference bound = walker.referenceBoundTo(operation.operand(operandIndex));
        if (!bound || !bound.isRecord() || recordBehindNamesAndFormsInQuotients(code, bound.id()) != recordBehindNamesAndFormsInQuotients(code, record.operand(operandIndex)))
            return false;
    }
    return true;
}

// Walker::referenceOf() gives an operand the narrowest name that holds where the walk stands.
static std::optional<BodyValue> findOperatorOnExactIntegersInQuotients(Walker& walker, Opcode opcode, ValueID number, const InputsOfTypedOperation& inputs)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = number ? index.valueBehindCasts(number) : ValueID();
    if (!written || index.isParameter(written) || index.opcodeOf(written) != opcode)
        return std::nullopt;
    BodyValue operation = index.value(written);
    if (!isExactIntegerInQuotients(walker, inputs.typeOf(operation.operand(0))) || !isExactIntegerInQuotients(walker, inputs.typeOf(operation.operand(1))))
        return std::nullopt;
    // Asked ahead, an operator that is not walked yet stands in the stretch that is asked about, and is built in the arm of its reader.
    if (!operandsAreInSightInQuotients(walker, operation) && (!inputs.areOnlyTypes() || walker.referenceBoundTo(written)))
        return std::nullopt;
    return operation;
}

std::optional<DivisionAsWritten> findQuotientOfIntegers(Walker& walker, ValueID number, const InputsOfTypedOperation& inputs)
{
    std::optional<BodyValue> quotient = findOperatorOnExactIntegersInQuotients(walker, Opcode::Div, number, inputs);
    if (!quotient)
        return std::nullopt;
    return DivisionAsWritten { quotient->operand(0), quotient->operand(1) };
}

// A remainder that is proven an integer needs nobody to stand for it.
std::optional<WholeRemainder> findWholeRemainder(Walker& walker, ValueID number, const InputsOfTypedOperation& inputs)
{
    std::optional<BodyValue> remainder = findOperatorOnExactIntegersInQuotients(walker, Opcode::Mod, number, inputs);
    if (!remainder)
        return std::nullopt;
    TypeID typeOfNumber = inputs.typeOf(number);
    if (!typeOfNumber || walker.types().at(typeOfNumber).isInteger())
        return std::nullopt;
    Range divisors = walker.types().at(inputs.typeOf(remainder->operand(1))).integers;
    return WholeRemainder { remainder->operand(0), remainder->operand(1), divisors.contains(0) };
}

// ---- Division of integers

static bool isExactIntegerOfQuotient(const Type& type, int64_t limit)
{
    return type.isInteger() && type.integers.isSubsetOf({ -limit, limit });
}

static bool areBothKnownInQuotients(const Type& dividend, const Type& divisor)
{
    return dividend.integers.isSingleValue() && divisor.integers.isSingleValue();
}

// Dividing the integers and rounding gives what rounding the number gives.
static bool integersDivideAsNumbersDoInQuotients(QuotientRounding rounding, const Type& dividend, const Type& divisor)
{
    // Round is computed on twice the dividend plus the divisor.
    int64_t limit = rounding == QuotientRounding::Round ? maxExactInteger >> 2 : maxExactInteger;
    if (!isExactIntegerOfQuotient(dividend, limit) || !isExactIntegerOfQuotient(divisor, limit))
        return false;
    if (rounding == QuotientRounding::ToInt32)
        return true;

    // What a zero divides is no integer, and 0 divided by a negative number is -0, which every rounding keeps.
    Range dividends = dividend.integers;
    Range divisors = divisor.integers;
    if (divisors.contains(0) || (dividends.contains(0) && divisors.maximum < 0))
        return false;
    if (rounding == QuotientRounding::Floor)
        return true;
    // A quotient between -1 and 0 goes up to -0.
    Range quotients = quotientsRoundedEitherWay(dividends, divisors);
    return quotients.minimum >= 0 || quotients.maximum < 0;
}

static int64_t floorOfQuotientInQuotients(int64_t dividend, int64_t divisor)
{
    int64_t quotient = dividend / divisor;
    bool isRoundedUp = dividend % divisor && (dividend < 0) != (divisor < 0);
    return isRoundedUp ? quotient - 1 : quotient;
}

// integersDivideAsNumbersDoInQuotients() has said yes of the two.
static int64_t quotientOfKnownIntegersInQuotients(QuotientRounding rounding, int64_t dividend, int64_t divisor)
{
    switch (rounding) {
    case QuotientRounding::Floor:
        return floorOfQuotientInQuotients(dividend, divisor);
    case QuotientRounding::Ceil:
        return -floorOfQuotientInQuotients(-dividend, divisor);
    case QuotientRounding::Truncate:
        return dividend / divisor;
    case QuotientRounding::Round:
        return floorOfQuotientInQuotients(2 * dividend + divisor, 2 * divisor);
    case QuotientRounding::ToInt32:
        // What a zero divides is an infinity or NaN, which converts to 0.
        return divisor ? static_cast<int32_t>(static_cast<uint64_t>(dividend / divisor)) : 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool quotientIsDividendInQuotients(QuotientRounding rounding, const Type& dividend, const Type& divisor)
{
    return divisor.integers == Range::one(1) && (rounding != QuotientRounding::ToInt32 || dividend.isInt32());
}

Reference buildDivisionOfIntegers(Walker& walker, Origin origin, Reference dividend, Reference divisor, QuotientRounding rounding)
{
    Type dividendType = typeOfBuiltInQuotients(walker, dividend);
    Type divisorType = typeOfBuiltInQuotients(walker, divisor);
    if (!integersDivideAsNumbersDoInQuotients(rounding, dividendType, divisorType))
        return { };
    if (areBothKnownInQuotients(dividendType, divisorType))
        return integerInNumberForms(walker, quotientOfKnownIntegersInQuotients(rounding, dividendType.integers.minimum, divisorType.integers.minimum));
    if (quotientIsDividendInQuotients(rounding, dividendType, divisorType))
        return dividend;

    Draft division(Opcode::DivideIntegers, origin);
    walker.chooseConservativeFields(division);
    division.setDividendOperand(dividend);
    division.setDivisorOperand(divisor);
    division.setRounding(rounding);
    return walker.build(WTF::move(division));
}

TypeID typeOfDivisionOfIntegers(Walker& walker, QuotientRounding rounding, TypeID dividend, TypeID divisor)
{
    if (!dividend || !divisor)
        return { };
    TypeArena& types = walker.types();
    Type dividendType = types.at(dividend);
    Type divisorType = types.at(divisor);
    if (!integersDivideAsNumbersDoInQuotients(rounding, dividendType, divisorType))
        return { };
    if (areBothKnownInQuotients(dividendType, divisorType))
        return types.integer(Range::one(quotientOfKnownIntegersInQuotients(rounding, dividendType.integers.minimum, divisorType.integers.minimum)));
    if (quotientIsDividendInQuotients(rounding, dividendType, divisorType))
        return dividend;

    Draft division(Opcode::DivideIntegers);
    division.setRounding(rounding);
    std::array<TypeID, 2> operands { dividend, divisor };
    return resultTypeOf(walker.typing(), division, OperandTypes { operands });
}

// ---- The whole remainder

TypeID typeOfWholeRemainder(Walker& walker, TypeID dividend, TypeID divisor)
{
    if (!isExactIntegerInQuotients(walker, dividend) || !isExactIntegerInQuotients(walker, divisor))
        return { };
    Draft whole(Opcode::RemainderOfIntegers);
    std::array<TypeID, 2> operands { dividend, divisor };
    return resultTypeOf(walker.typing(), whole, OperandTypes { operands });
}

// It answers for every divisor, so whether it may stand for the number is the asker's to know.
Reference buildWholeRemainder(Walker& walker, Origin origin, const WholeRemainder& remainder)
{
    Reference dividend = walker.referenceOf(remainder.dividend);
    Reference divisor = walker.referenceOf(remainder.divisor);
    Range dividends = typeOfBuiltInQuotients(walker, dividend).integers;
    Range divisors = typeOfBuiltInQuotients(walker, divisor).integers;

    Draft whole(Opcode::RemainderOfIntegers, origin);
    walker.chooseConservativeFields(whole);
    whole.setDividendOperand(dividend);
    whole.setDivisorOperand(divisor);

    // A divisor that is 0 leaves 0, by the rule of the row.
    Type result = walker.types().at(walker.resultTypeOf(whole));
    if (result.isInteger() && result.integers.isSingleValue())
        return integerInNumberForms(walker, result.integers.minimum);
    if (dividends.isSingleValue() && divisors.isSingleValue())
        return integerInNumberForms(walker, dividends.minimum % divisors.minimum);

    // A dividend that lies nearer to zero than every divisor is left whole.
    int64_t leastMagnitudeOfDivisor = divisors.minimum > 0 ? divisors.minimum : -divisors.maximum;
    if (leastMagnitudeOfDivisor > 0 && dividends.minimum > -leastMagnitudeOfDivisor && dividends.maximum < leastMagnitudeOfDivisor)
        return dividend;
    return walker.build(WTF::move(whole));
}

// Nothing: the divisor is no constant, or no power of two that a mask of an int32 has room for.
static std::optional<int64_t> powerOfTwoDividingWholeRemainder(Walker& walker, const WholeRemainder& remainder)
{
    std::optional<int64_t> divisor = integerKnownInNumberForms(walker, walker.referenceOf(remainder.divisor));
    if (!divisor)
        return std::nullopt;
    int64_t magnitude = std::abs(*divisor);
    if (!magnitude || magnitude > int64_t { 1 } << 30 || (magnitude & (magnitude - 1)))
        return std::nullopt;
    return magnitude;
}

static Reference buildBitsOfDividendOfWholeRemainder(Walker& walker, Origin origin, const WholeRemainder& remainder, int32_t mask)
{
    return buildOperatorInNumberForms(walker, Opcode::BitAnd, walker.referenceOf(remainder.dividend), integerInNumberForms(walker, mask), origin);
}

// ToInt32 keeps the low bits of an integer of any size.
Reference buildZeroWhereWholeRemainderIsZero(Walker& walker, Origin origin, const WholeRemainder& remainder)
{
    if (std::optional<int64_t> divisor = powerOfTwoDividingWholeRemainder(walker, remainder))
        return buildBitsOfDividendOfWholeRemainder(walker, origin, remainder, static_cast<int32_t>(*divisor - 1));
    return buildWholeRemainder(walker, origin, remainder);
}

std::optional<BitsOfDividendCompared> tryCompareBitsOfDividend(Walker& walker, Origin origin, const WholeRemainder& remainder, int64_t comparedWith)
{
    std::optional<int64_t> divisor = powerOfTwoDividingWholeRemainder(walker, remainder);
    if (!divisor || std::abs(comparedWith) >= *divisor)
        return std::nullopt;
    uint32_t lowBits = static_cast<uint32_t>(*divisor - 1);
    if (!comparedWith)
        return BitsOfDividendCompared { buildBitsOfDividendOfWholeRemainder(walker, origin, remainder, static_cast<int32_t>(lowBits)), integerInNumberForms(walker, 0) };

    // The remainder has the sign of the dividend, and the sign of an int32 is its highest bit.
    if (!typeOfBuiltInQuotients(walker, walker.referenceOf(remainder.dividend)).isInt32())
        return std::nullopt;
    constexpr uint32_t sign = 1u << 31;
    uint32_t bitsCompared = (comparedWith < 0 ? sign : 0) | (static_cast<uint32_t>(comparedWith) & lowBits);
    return BitsOfDividendCompared {
        buildBitsOfDividendOfWholeRemainder(walker, origin, remainder, static_cast<int32_t>(sign | lowBits)),
        integerInNumberForms(walker, static_cast<int32_t>(bitsCompared))
    };
}

Reference buildComparisonWhereDivisorsAreNotZero(Walker& walker, Draft&& comparison, std::span<const ValueID> divisorsThatMayBeZero)
{
    Origin origin = comparison.origin();
    origin.isTailCall = false;

    // Behind the first branch stands another block, so every test is built in front of it.
    Vector<Reference, 2> testsThatDivisorsAreNotZero;
    for (ValueID written : divisorsThatMayBeZero) {
        Reference divisor = walker.referenceOf(written);
        Range divisors = typeOfBuiltInQuotients(walker, divisor).integers;
        if (divisors == Range::one(0))
            return booleanInNumberForms(walker, false);
        if (!divisors.contains(0))
            continue;
        Reference isNotZero = buildOperatorInNumberForms(walker, Opcode::ToBoolean, divisor, { }, origin);
        if (!isNotZero)
            return { };
        testsThatDivisorsAreNotZero.append(isNotZero);
    }
    if (testsThatDivisorsAreNotZero.isEmpty())
        return buildInAllowedFormInNumberForms(walker, WTF::move(comparison));

    // A program that divides by zero is as rare as one whose sums pass int32.
    BlockID whereSomeDivisorIsZero = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    for (Reference isNotZero : testsThatDivisorsAreNotZero)
        goOnWhereTestPassesInNumberForms(walker, isNotZero, whereSomeDivisorIsZero);
    Reference compared = buildInAllowedFormInNumberForms(walker, WTF::move(comparison));
    BlockID endOfComparison = compared ? walker.currentBlock() : BlockID();

    walker.continueIn(whereSomeDivisorIsZero);
    std::array<WayInNumberForms, 2> ways { {
        { endOfComparison, compared, { } },
        { walker.currentBlock(), booleanInNumberForms(walker, false), { } },
    } };
    return meetInNumberForms(walker, meeting, ways);
}

// ---- A remainder of a number that is only tested

// The input puts a proof of the tested value on both edges of every branch on its test, whether or not anybody reads the parameter that takes it, and each proof counts as a mention of the value.
// How many of them the edges behind the test hand on. Nothing where one is read: there the value goes on under another name.
static std::optional<uint32_t> numberOfUnreadProofsBehindTestInQuotients(const BodyIndex& index, ValueID test, ValueID subject)
{
    std::optional<BodyUser> user = index.onlyUser(test);
    if (!user || user->kind != BodyUseKind::TerminatorOperand)
        return 0;

    uint32_t numberOfProofs = 0;
    bool someProofIsRead = false;
    index.forEachEdgeResult(user->block, [&](const BodyEdgeResult& result) {
        if (result.subject != subject)
            return;
        ++numberOfProofs;
        someProofIsRead |= !index.isUnused(result.parameter);
    });
    if (someProofIsRead)
        return std::nullopt;
    return numberOfProofs;
}

// Two arms stand in the place of one operation, which pays wherever the code comes again.
static bool codeComesAgainInQuotients(const Walker& walker)
{
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.unit().function;
    return !walker.facts().runsAtMostOnce(function) && !walker.program().at(function).isModuleBody();
}

// A dividend that is an integer is divided as one, and the integer that is left is tested. Whatever else it is, -0 among it, is divided as a number. Neither arm guesses: both are right.
bool buildNumberRemainderWithItsTest(Walker& walker, const BodyValue& remainder)
{
    if (remainder.opcode() != Opcode::Mod || !walker.opensGuesses() || !codeComesAgainInQuotients(walker))
        return false;
    ValueID writtenDividend = remainder.operand(0);
    ValueID writtenDivisor = remainder.operand(1);
    Reference dividend = walker.referenceOf(writtenDividend);
    Reference divisor = walker.referenceOf(writtenDivisor);
    std::optional<int64_t> knownDivisor = integerKnownInNumberForms(walker, divisor);
    if (!knownDivisor || *knownDivisor < 2 || *knownDivisor > std::numeric_limits<int32_t>::max() || !dividend.isValue())
        return false;
    Type typeOfDividend = typeOfBuiltInQuotients(walker, dividend);
    if (!typeOfDividend.isNumber() || typeOfDividend.isInteger() || !(typeOfDividend.primitives & Primitive::Integer))
        return false;

    // By the body: nothing reads the remainder but one ToBoolean, or one equality with an integer that is written there.
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<BodyUser> firstMention = index.firstUser(remainder.id());
    if (!firstMention || firstMention->kind != BodyUseKind::Operand)
        return false;
    BodyValue test = index.value(firstMention->value);
    BodyBlock block = index.block(index.blockOf(remainder.id()));
    uint32_t firstValue = block.firstValue().index();
    if (!block.holds(test.id()) || test.id().index() >= firstValue + walker.endOfStretchBeingWalked())
        return false;
    std::optional<uint32_t> unreadProofs = numberOfUnreadProofsBehindTestInQuotients(index, test.id(), remainder.id());
    if (!unreadProofs || 1 + *unreadProofs >= BodyIndex::maxCountedUses || index.useCount(remainder.id()) != 1 + *unreadProofs)
        return false;

    std::optional<int64_t> comparedWith;
    if (test.opcode() != Opcode::ToBoolean) {
        if (test.opcode() != Opcode::StrictEq && test.opcode() != Opcode::Eq)
            return false;
        ValueID other = test.operand(0) == remainder.id() ? test.operand(1) : test.operand(0);
        if (!other || index.isParameter(other) || index.opcodeOf(other) != Opcode::Integer)
            return false;
        comparedWith = index.value(other).as<Opcode::Integer>().integer();
    }

    // findWholeRemainder() takes no integer further from zero.
    TypeID whole = walker.types().integer(Range::exact());
    if (walker.types().testablePartOf(whole) != whole)
        return false;

    // The remainder of two numbers is pure, so it may stand behind whatever is written between it and its test.
    uint32_t placeOfTest = test.id().index() - firstValue;
    walker.walkStretch(block.id(), remainder.id().index() + 1 - firstValue, placeOfTest);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;

    Origin origin = walker.originOf(remainder);
    origin.isTailCall = false;
    Draft isWhole(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(isWhole);
    isWhole.setValueOperand(dividend);
    isWhole.setTestedType(whole);
    Reference dividendIsWhole = walker.build(WTF::move(isWhole));
    if (!dividendIsWhole)
        return true;

    BlockID onIntegers = walker.addBlock();
    BlockID onNumbers = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    walker.branch(dividendIsWhole, onIntegers, onNumbers);

    auto endOfArm = [&](Reference left, Reference answer) {
        return WayInNumberForms { left && answer ? walker.currentBlock() : BlockID(), answer, left };
    };

    walker.continueIn(onIntegers);
    WayInNumberForms wayOfIntegers { };
    if (Reference integer = buildNarrowedInNumberForms(walker, dividend, dividendIsWhole, OperandIndex::TypeTest::value, true, origin)) {
        Reference bound = walker.referenceBoundTo(writtenDividend);
        walker.bind(writtenDividend, integer);
        Reference left = buildOperatorInNumberForms(walker, Opcode::Mod, integer, divisor, origin);
        WholeRemainder wholeRemainder { writtenDividend, writtenDivisor, false };
        Reference answer;
        if (!comparedWith)
            answer = buildOperatorInNumberForms(walker, Opcode::ToBoolean, buildZeroWhereWholeRemainderIsZero(walker, origin, wholeRemainder), { }, origin);
        else if (std::optional<BitsOfDividendCompared> bits = tryCompareBitsOfDividend(walker, origin, wholeRemainder, *comparedWith))
            answer = buildOperatorInNumberForms(walker, Opcode::StrictEq, bits->bits, bits->comparedWith, origin);
        else
            answer = buildOperatorInNumberForms(walker, Opcode::StrictEq, buildWholeRemainder(walker, origin, wholeRemainder), integerInNumberForms(walker, *comparedWith), origin);
        walker.bind(writtenDividend, bound);
        wayOfIntegers = endOfArm(left, answer);
    }

    walker.continueIn(onNumbers);
    Reference left = buildOperatorInNumberForms(walker, Opcode::Mod, dividend, divisor, origin);
    Reference answer;
    if (left && !comparedWith)
        answer = buildOperatorInNumberForms(walker, Opcode::ToBoolean, left, { }, origin);
    else if (left)
        answer = buildOperatorInNumberForms(walker, Opcode::StrictEq, left, integerInNumberForms(walker, *comparedWith), origin);
    std::array<WayInNumberForms, 2> ways { wayOfIntegers, endOfArm(left, answer) };

    Reference metRemainder;
    Reference metAnswer = meetInNumberForms(walker, meeting, ways, &metRemainder);
    if (!metAnswer)
        return true;
    walker.bind(remainder.id(), metRemainder);
    walker.bind(test.id(), metAnswer);
    // No operand runs code, so a call that Link lists at the test for a conversion enters nothing. It is told all the same, as Walker::walkOperation() tells it of a test that it walks.
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), test.id()))
        walker.arrivals().leaveAnything(callee);
    walker.walkStretch(block.id(), placeOfTest + 1, placeOfTest + 1);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
