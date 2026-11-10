/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static bool isExactIntegerInQuotients(Walker& walker, TypeID id)
{
    if (!id)
        return false;
    Type type = walker.types().at(id);
    return type.isInteger() && type.integers.isSubsetOf(Range::exact());
}

static ValueID recordBehindNamesAndFormsInQuotients(const Code& code, ValueID record)
{
    while (cellPreservingOpcodes.contains(code.at(record).opcode))
        record = code.at(record).operand(0);
    return record;
}

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

static ALWAYS_INLINE std::optional<BodyValue> findOperatorOnExactIntegersInQuotients(Walker& walker, Opcode opcode, ValueID number, const InputsOfTypedOperation& inputs)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = number ? index.stripCasts(number) : ValueID();
    if (!written || index.isParameter(written) || index.opcodeOf(written) != opcode)
        return std::nullopt;
    BodyValue operation = index.value(written);
    if (!isExactIntegerInQuotients(walker, inputs.typeOf(operation.operand(0))) || !isExactIntegerInQuotients(walker, inputs.typeOf(operation.operand(1))))
        return std::nullopt;
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

static bool isExactIntegerOfQuotient(const Type& type, int64_t limit)
{
    return type.isInteger() && type.integers.isSubsetOf({ -limit, limit });
}

static bool areBothKnownInQuotients(const Type& dividend, const Type& divisor)
{
    return dividend.integers.isSingleValue() && divisor.integers.isSingleValue();
}

static bool integersDivideAsNumbersDoInQuotients(QuotientRounding rounding, const Type& dividend, const Type& divisor)
{
    int64_t limit = rounding == QuotientRounding::Round ? maxExactInteger >> 2 : maxExactInteger;
    if (!isExactIntegerOfQuotient(dividend, limit) || !isExactIntegerOfQuotient(divisor, limit))
        return false;
    if (rounding == QuotientRounding::ToInt32)
        return true;

    Range dividends = dividend.integers;
    Range divisors = divisor.integers;
    if (divisors.contains(0) || (dividends.contains(0) && divisors.maximum < 0))
        return false;
    if (rounding == QuotientRounding::Floor)
        return true;
    Range quotients = quotientsRoundedEitherWay(dividends, divisors);
    return quotients.minimum >= 0 || quotients.maximum < 0;
}

static int64_t floorOfQuotientInQuotients(int64_t dividend, int64_t divisor)
{
    int64_t quotient = dividend / divisor;
    bool isRoundedUp = dividend % divisor && (dividend < 0) != (divisor < 0);
    return isRoundedUp ? quotient - 1 : quotient;
}

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

TypeID typeOfWholeRemainder(Walker& walker, TypeID dividend, TypeID divisor)
{
    if (!isExactIntegerInQuotients(walker, dividend) || !isExactIntegerInQuotients(walker, divisor))
        return { };
    Draft whole(Opcode::RemainderOfIntegers);
    std::array<TypeID, 2> operands { dividend, divisor };
    return resultTypeOf(walker.typing(), whole, OperandTypes { operands });
}

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

    Type result = walker.types().at(walker.resultTypeOf(whole));
    if (result.isInteger() && result.integers.isSingleValue())
        return integerInNumberForms(walker, result.integers.minimum);
    if (dividends.isSingleValue() && divisors.isSingleValue())
        return integerInNumberForms(walker, dividends.minimum % divisors.minimum);

    int64_t leastMagnitudeOfDivisor = divisors.minimum > 0 ? divisors.minimum : -divisors.maximum;
    if (leastMagnitudeOfDivisor > 0 && dividends.minimum > -leastMagnitudeOfDivisor && dividends.maximum < leastMagnitudeOfDivisor)
        return dividend;
    return walker.build(WTF::move(whole));
}

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

    BlockID whereSomeDivisorIsZero = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    for (Reference isNotZero : testsThatDivisorsAreNotZero)
        continueIfTrue(walker, isNotZero, whereSomeDivisorIsZero);
    Reference compared = buildInAllowedFormInNumberForms(walker, WTF::move(comparison));
    BlockID endOfComparison = compared ? walker.currentBlock() : BlockID();

    walker.continueIn(whereSomeDivisorIsZero);
    std::array<WayInNumberForms, 2> ways { {
        { endOfComparison, compared, { } },
        { walker.currentBlock(), booleanInNumberForms(walker, false), { } },
    } };
    return meetInNumberForms(walker, meeting, ways);
}

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

static bool codeComesAgainInQuotients(const Walker& walker)
{
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.unit().function;
    return !walker.program().at(function).isModuleBody();
}

namespace {

struct TestOfRemainder {
    ValueID test;
    std::optional<int64_t> comparedWith;
};

} // anonymous namespace

static std::optional<TestOfRemainder> findTestInQuotients(Walker& walker, const BodyValue& remainder, std::optional<int64_t> divisor)
{
    if (!divisor || *divisor < 2 || *divisor > std::numeric_limits<int32_t>::max())
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<BodyUser> firstMention = index.firstUser(remainder.id());
    if (!firstMention || firstMention->kind != BodyUseKind::Operand)
        return std::nullopt;
    BodyValue test = index.value(firstMention->value);
    BodyBlock block = index.block(index.blockOf(remainder.id()));
    if (!block.holds(test.id()) || test.id().index() >= block.firstValue().index() + walker.endOfStretchBeingWalked())
        return std::nullopt;
    std::optional<uint32_t> unreadProofs = numberOfUnreadProofsBehindTestInQuotients(index, test.id(), remainder.id());
    if (!unreadProofs || 1 + *unreadProofs >= BodyIndex::maxCountedUses || index.useCount(remainder.id()) != 1 + *unreadProofs)
        return std::nullopt;

    if (test.opcode() == Opcode::ToBoolean)
        return TestOfRemainder { test.id(), std::nullopt };
    if (test.opcode() != Opcode::StrictEq && test.opcode() != Opcode::Eq)
        return std::nullopt;
    ValueID other = test.operand(0) == remainder.id() ? test.operand(1) : test.operand(0);
    if (!other || index.isParameter(other) || index.opcodeOf(other) != Opcode::Integer)
        return std::nullopt;
    return TestOfRemainder { test.id(), index.value(other).as<Opcode::Integer>().integer() };
}

ValueID findTestOfRemainder(Walker& walker, const BodyValue& remainder)
{
    if (remainder.opcode() != Opcode::Mod || !walker.opensGuesses() || !walker.isInLoop())
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID divisor = index.stripCasts(remainder.operand(1));
    if (!divisor || index.isParameter(divisor) || index.opcodeOf(divisor) != Opcode::Integer)
        return { };
    std::optional<TestOfRemainder> found = findTestInQuotients(walker, remainder, index.value(divisor).as<Opcode::Integer>().integer());
    return found ? found->test : ValueID();
}

bool buildNumberRemainderWithItsTest(Walker& walker, const BodyValue& remainder)
{
    if (remainder.opcode() != Opcode::Mod || !walker.opensGuesses() || !codeComesAgainInQuotients(walker))
        return false;
    ValueID writtenDividend = remainder.operand(0);
    ValueID writtenDivisor = remainder.operand(1);
    Reference dividend = walker.referenceOf(writtenDividend);
    Reference divisor = walker.referenceOf(writtenDivisor);
    if (!dividend.isValue())
        return false;
    Type typeOfDividend = typeOfBuiltInQuotients(walker, dividend);
    if (!typeOfDividend.isNumber() || typeOfDividend.isInteger() || !(typeOfDividend.primitives & Primitive::Integer))
        return false;

    std::optional<TestOfRemainder> found = findTestInQuotients(walker, remainder, integerKnownInNumberForms(walker, divisor));
    if (!found)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyValue test = index.value(found->test);
    BodyBlock block = index.block(index.blockOf(remainder.id()));
    uint32_t firstValue = block.firstValue().index();
    std::optional<int64_t> comparedWith = found->comparedWith;

    TypeID whole = walker.types().integer(Range::exact());
    if (walker.types().testablePartOf(whole) != whole)
        return false;

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
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), test.id()))
        walker.leaveAnythingAt(callee);
    walker.walkStretch(block.id(), placeOfTest + 1, placeOfTest + 1);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
