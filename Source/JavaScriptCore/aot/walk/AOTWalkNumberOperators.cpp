/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTWalkNumberGuesses.h"
#include "JSGlobalObjectFunctions.h"
#include <array>
#include <cmath>


namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet arithmeticInNumberOperators { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow };
static constexpr OpcodeSet opcodesOfNumberOperators = arithmeticInNumberOperators | allBitwiseOperators | allEqualities | allRelations
    | OpcodeSet { Opcode::LogicalNot, Opcode::ToBoolean };

namespace {

struct OperandInNumberOperators {
    ValueID written;
    Reference reference;
    Type type;
    PrimitiveKnownToFolding known;
};

using OperandsInNumberOperators = std::array<OperandInNumberOperators, 2>;

} // anonymous namespace

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
        operand.known = primitiveKnownInNumberForms(walker, operand.reference, operand.type);
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

static bool bindWhatIsNegatedTwice(Walker& walker, const BodyValue& negation)
{
    if (!walker.has(Mechanism::Folding))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID inner = index.stripCasts(negation.valueOperand());
    if (inner && !index.isParameter(inner) && index.opcodeOf(inner) == Opcode::ToBoolean)
        inner = index.stripCasts(index.value(inner).valueOperand());
    if (!inner || index.isParameter(inner) || index.opcodeOf(inner) != Opcode::LogicalNot)
        return false;
    Reference negatedTwice = walker.referenceOf(index.value(inner).valueOperand());
    TypeID type = negatedTwice && negatedTwice.isRecord() ? walker.typeOf(negatedTwice) : TypeID();
    if (!type || !walker.types().at(type).isBoolean())
        return false;
    walker.bind(negation.id(), negatedTwice);
    return true;
}

static Reference valueThatArithmeticGives(Walker& walker, Opcode opcode, const OperandsInNumberOperators& operands)
{
    const OperandInNumberOperators& left = operands[0];
    const OperandInNumberOperators& right = operands[1];
    switch (opcode) {
    case Opcode::Add:
        for (uint32_t side = 0; side < 2; ++side) {
            const OperandInNumberOperators& other = operands[1 - side];
            if (isKnownIntegerInNumberOperators(operands[side].known, 0) && other.type.isInteger())
                return other.reference;
            if (isNegativeZeroInNumberOperators(operands[side].known) && other.type.isNumber())
                return other.reference;
        }
        return { };
    case Opcode::Sub:
        if (isKnownIntegerInNumberOperators(right.known, 0) && left.type.isNumber())
            return left.reference;
        if (isNegativeZeroInNumberOperators(right.known) && left.type.isInteger())
            return left.reference;
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
        if (isKnownIntegerInNumberOperators(right.known, 0) || isNegativeZeroInNumberOperators(right.known))
            return integerInNumberForms(walker, 1);
        return isKnownIntegerInNumberOperators(right.known, 1) ? left.reference : Reference();
    default:
        return { };
    }
}

static Reference buildMiddleOfBounds(Walker& walker, const BodyValue& sum, const OperandsInNumberOperators& operands)
{
    static constexpr Range halfOfInt32Range { -(1ll << 30), (1ll << 30) - 1 };
    const BodyIndex& index = walker.currentBodyIndex();
    auto findWritten = [&](ValueID operand, Opcode opcode) -> ValueID {
        ValueID written = operand ? index.stripCasts(operand) : ValueID();
        return written && !index.isParameter(written) && index.opcodeOf(written) == opcode ? written : ValueID();
    };
    auto staysInHalf = [&](Reference bound) {
        TypeID type = bound && bound.isRecord() ? walker.typeOf(bound) : TypeID();
        return type && walker.types().at(type).isInteger() && !walker.types().at(type).integers.isEmpty() && walker.types().at(type).integers.isSubsetOf(halfOfInt32Range);
    };
    for (uint32_t side = 0; side < 2; ++side) {
        ValueID shift = findWritten(operands[1 - side].written, Opcode::RShift);
        ValueID places = shift ? findWritten(index.value(shift).operand(1), Opcode::Integer) : ValueID();
        ValueID difference = places && index.value(places).as<Opcode::Integer>().integer() == 1 ? findWritten(index.value(shift).operand(0), Opcode::Sub) : ValueID();
        if (!difference)
            continue;
        Reference low = operands[side].reference;
        Reference high = walker.referenceOf(index.value(difference).operand(0));
        if (walker.referenceOf(index.value(difference).operand(1)) != low || !staysInHalf(low) || !staysInHalf(high))
            continue;
        Origin origin = walker.originOf(sum);
        Reference doubled = buildOperatorInNumberForms(walker, Opcode::Add, low, high, origin);
        return doubled ? buildOperatorInNumberForms(walker, Opcode::RShift, doubled, integerInNumberForms(walker, 1), origin) : Reference();
    }
    return { };
}

static Reference buildConversionInNumberOperators(Walker&, Opcode conversion, Reference number, Origin);

static Reference buildOptionalIntegerSum(Walker& walker, Opcode opcode, std::array<Reference, 2> terms, Origin origin)
{
    TypeArena& types = walker.types();
    uint32_t numberOfOptionalTerms = 0;
    for (Reference term : terms) {
        TypeID type = term && term.isRecord() ? walker.typeOf(term) : TypeID();
        if (type && types.isSubset(type, TypeTable::int32))
            continue;
        if (!type || !types.overlap(type, TypeTable::int32) || !types.isSubset(type, types.withPrimitives(TypeTable::int32, Primitive::Undefined)))
            return { };
        ++numberOfOptionalTerms;
    }
    if (!numberOfOptionalTerms)
        return { };

    Vector<BlockID, 2> undefinedBlocks;
    for (Reference& term : terms) {
        if (types.isSubset(walker.typeOf(term), TypeTable::int32))
            continue;
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(term);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Undefined));
        Reference isUndefined = walker.build(WTF::move(test));
        bool isTested = isUndefined && isUndefined.isValue() && !walker.code().isConstant(isUndefined.id()) && !walker.isUnreachable();
        if (!isTested)
            return { };
        BlockID integerBlock = walker.addBlock();
        undefinedBlocks.append(walker.addBlock(BlockFrequency::Unlikely));
        walker.branch(isUndefined, undefinedBlocks.last(), integerBlock);
        walker.continueIn(integerBlock);
        term = buildNarrowedInNumberForms(walker, term, isUndefined, OperandIndex::TypeTest::value, false, origin);
        if (!term)
            return { };
    }
    walker.count(StatisticID::SumsWithUndefinedTruncated);
    BlockID meeting = walker.addBlock();
    Reference added = buildOperatorInNumberForms(walker, opcode, terms[0], terms[1], origin);
    Reference bits = added ? buildConversionInNumberOperators(walker, Opcode::ToInt32, added, origin) : Reference();
    if (!bits)
        return { };
    Vector<WayInNumberForms, 3> ways;
    ways.append(WayInNumberForms { walker.currentBlock(), bits, { } });
    for (BlockID block : undefinedBlocks) {
        walker.continueIn(block);
        ways.append(WayInNumberForms { walker.currentBlock(), integerInNumberForms(walker, 0), { } });
    }
    return meetInNumberForms(walker, meeting, ways.span());
}

namespace {

struct TermOfTruncatedSum {
    Reference value;
    bool isSubtracted { false };
};

} // anonymous namespace

static Reference buildSumOfOptionalIntegers(Walker& walker, Origin origin, ValueID sum)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    static constexpr size_t mostTerms = 4;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    TypeID int32OrUndefined = types.withPrimitives(TypeTable::int32, Primitive::Undefined);
    BlockID block = index.blockOf(sum);
    Vector<TermOfTruncatedSum, mostTerms> terms;
    bool hasOtherTerm = false;
    auto gather = [&](auto& self, ValueID operand, bool isSubtracted, bool mayBeSum) -> void {
        ValueID written = operand;
        bool isUsedOnce = mayBeSum;
        while (written && !index.isParameter(written) && opcodesThatStateTypes.contains(index.opcodeOf(written))) {
            isUsedOnce &= index.hasOneUse(written);
            written = index.value(written).operand(0);
        }
        if (isUsedOnce && written && !index.isParameter(written) && sums.contains(index.opcodeOf(written)) && index.blockOf(written) == block && (written == sum || index.hasOneUse(written)) && terms.size() < mostTerms) {
            self(self, index.value(written).operand(0), isSubtracted, true);
            self(self, index.value(written).operand(1), isSubtracted ^ (index.opcodeOf(written) == Opcode::Sub), true);
            return;
        }
        Reference value = walker.referenceOf(operand);
        TypeID type = value && value.isRecord() ? walker.typeOf(value) : TypeID();
        hasOtherTerm |= !type || terms.size() == mostTerms || !types.overlap(type, TypeTable::int32) || !types.isSubset(type, int32OrUndefined);
        if (!hasOtherTerm)
            terms.append(TermOfTruncatedSum { value, isSubtracted });
    };
    gather(gather, sum, false, true);
    auto mayBeUndefined = [&](const TermOfTruncatedSum& term) {
        return !types.isSubset(walker.typeOf(term.value), TypeTable::int32);
    };
    size_t numberOfOptionalTerms = std::ranges::count_if(terms, mayBeUndefined);
    if (hasOtherTerm || terms.size() < 2 || !numberOfOptionalTerms || (terms.size() == 2 && numberOfOptionalTerms == 1))
        return { };

    Vector<BlockID, mostTerms> undefinedBlocks;
    for (size_t place = 0; place < terms.size(); ++place) {
        if (!mayBeUndefined(terms[place]))
            continue;
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(terms[place].value);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Undefined));
        Reference isUndefined = walker.build(WTF::move(test));
        if (!isUndefined || walker.isUnreachable())
            return { };
        if (!isUndefined.isValue() || walker.code().isConstant(isUndefined.id()))
            continue;
        BlockID integerBlock = walker.addBlock();
        undefinedBlocks.append(walker.addBlock(BlockFrequency::Unlikely));
        walker.branch(isUndefined, undefinedBlocks.last(), integerBlock);
        walker.continueIn(integerBlock);
        Reference before = terms[place].value;
        Reference integer = buildNarrowedInNumberForms(walker, before, isUndefined, OperandIndex::TypeTest::value, false, origin);
        if (!integer)
            return { };
        for (TermOfTruncatedSum& term : terms) {
            if (term.value == before)
                term.value = integer;
        }
    }
    walker.count(StatisticID::SumsWithUndefinedTruncated);
    BlockID meeting = walker.addBlock();
    if (terms[0].isSubtracted) {
        auto added = std::ranges::find_if(terms, [](const TermOfTruncatedSum& term) { return !term.isSubtracted; });
        if (added != terms.end())
            std::swap(terms[0], *added);
    }
    Reference total = terms[0].isSubtracted ? buildOperatorInNumberForms(walker, Opcode::Sub, integerInNumberForms(walker, 0), terms[0].value, origin) : terms[0].value;
    for (size_t place = 1; place < terms.size() && total; ++place)
        total = buildOperatorInNumberForms(walker, terms[place].isSubtracted ? Opcode::Sub : Opcode::Add, total, terms[place].value, origin);
    Reference bits = total ? buildConversionInNumberOperators(walker, Opcode::ToInt32, total, origin) : Reference();
    if (!bits)
        return { };
    Vector<WayInNumberForms, 5> ways;
    ways.append(WayInNumberForms { walker.currentBlock(), bits, { } });
    for (BlockID undefinedBlock : undefinedBlocks) {
        walker.continueIn(undefinedBlock);
        ways.append(WayInNumberForms { walker.currentBlock(), integerInNumberForms(walker, 0), { } });
    }
    return meetInNumberForms(walker, meeting, ways.span());
}

static Reference buildSumWithUndefined(Walker& walker, Origin origin, ValueID written)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID sum = index.stripCasts(written);
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::UnboxedNumbers) || index.isParameter(sum) || !sums.contains(index.opcodeOf(sum)))
        return { };
    std::array<Reference, 2> terms { walker.referenceOf(index.value(sum).operand(0)), walker.referenceOf(index.value(sum).operand(1)) };
    auto mayBeUndefined = [&](Reference term) {
        return !term || !term.isRecord() || walker.types().at(walker.typeOf(term)).primitives & Primitive::Undefined;
    };
    if (Reference bits = buildSumOfOptionalIntegers(walker, origin, sum); bits || walker.hasFailed() || walker.isUnreachable())
        return bits;
    if (mayBeUndefined(terms[0]) && mayBeUndefined(terms[1]))
        return { };
    return buildOptionalIntegerSum(walker, index.opcodeOf(sum), terms, origin);
}

Reference buildTruncatedSum(Walker& walker, Reference sum, Origin origin)
{
    const Code& code = walker.code();
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::UnboxedNumbers) || !sum.isValue() || code.isConstant(sum.id()))
        return { };
    const Operation& record = code.at(sum.id());
    if ((record.opcode != Opcode::Add && record.opcode != Opcode::Sub) || record.representation != Representation::Double || record.form() != OperatorForm::Numbers)
        return { };
    std::array<Reference, 2> terms;
    for (uint32_t side = 0; side < 2; ++side) {
        ValueID term = record.operand(side);
        while (!code.isConstant(term) && conversionsOfForm.contains(code.at(term).opcode))
            term = code.at(term).operand(0);
        bool isInteger = code.isConstant(term) || code.at(term).representation == Representation::Int32 || code.at(term).representation == Representation::Int64 || code.at(term).representation == Representation::OptionalInt64;
        if (!isInteger || (side && term == record.operand(0)))
            return { };
        terms[side] = Reference::value(term);
    }
    if (terms[0] == terms[1])
        return { };
    return buildOptionalIntegerSum(walker, record.opcode, terms, origin);
}

static Reference buildSimplerOperandOfBitOperator(Walker& walker, Origin origin, ValueID written)
{
    if (Reference bits = buildSumWithUndefined(walker, origin, written))
        return bits;
    if (walker.hasFailed() || walker.isUnreachable())
        return { };
    InputsOfTypedOperation inputs(walker, nullptr);
    if (std::optional<DivisionAsWritten> quotient = findQuotientOfIntegers(walker, written, inputs))
        return buildDivisionOfIntegers(walker, origin, quotient->dividend, quotient->divisor, QuotientRounding::ToInt32);
    if (std::optional<WholeRemainder> remainder = findWholeRemainder(walker, written, inputs))
        return buildWholeRemainder(walker, origin, *remainder);
    return { };
}

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

static Reference buildConversionInNumberOperators(Walker& walker, Opcode conversion, Reference number, Origin origin)
{
    Draft draft(conversion, origin);
    walker.chooseConservativeFields(draft);
    draft.setValueOperand(number);
    return walker.build(WTF::move(draft));
}

static Reference buildBitsOfNumberInNumberOperators(Walker& walker, Opcode conversion, Reference number, Origin origin)
{
    Type type = walker.types().at(walker.typeOf(number));
    Range leftAsTheyAre = conversion == Opcode::ToUint32 ? Range::uint32() : Range::int32();
    if (type.isInteger() && type.integers.isSubsetOf(leftAsTheyAre) && walker.has(Mechanism::Folding)) {
        walker.checkRawNumberThatFoldRestsOn(number, origin);
        return number;
    }
    if (conversion == Opcode::ToUint32 && !type.isInteger()) {
        number = buildConversionInNumberOperators(walker, Opcode::ToInt32, number, origin);
        if (!number)
            return { };
    }
    return buildConversionInNumberOperators(walker, conversion, number, origin);
}

static bool isBoxedNumberOrNullish(Walker& walker, Reference operand)
{
    Type type = walker.types().at(walker.typeOf(operand));
    return operand.isValue() && !type.isNumber() && (type.primitives & Primitive::Integer) && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull)
        && walker.code().at(operand.id()).representation == Representation::JSValue;
}

static Reference buildBitsOfNumberOrNullish(Walker& walker, Reference operand, Origin origin)
{
    Reference number;
    BlockID whereNullish = walker.splitNumberFromContainer(operand, number);
    if (!whereNullish && !walker.hasFailed()) {
        Reference isNumber = buildTestOfNumber(walker, operand, walker.types().withoutNullish(walker.typeOf(operand)), origin);
        if (!isNumber || !isNumber.isValue())
            return { };
        whereNullish = walker.addBlock(BlockFrequency::Rare);
        continueIfTrue(walker, isNumber, whereNullish);
        number = buildProofOfNumber(walker, operand, isNumber, origin);
    }
    Reference bits = number ? buildConversionInNumberOperators(walker, Opcode::ToInt32, number, origin) : Reference();
    if (!bits)
        return { };
    WayInNumberForms underNumber { walker.currentBlock(), bits, { } };
    walker.continueIn(whereNullish);
    WayInNumberForms underNullish { walker.currentBlock(), walker.constant(PrimitiveKnownToFolding::number(0)), { } };
    std::array<WayInNumberForms, 2> ways { underNumber, underNullish };
    return meetInNumberForms(walker, walker.addBlock(), ways);
}

static bool isRawIterationNumber(Walker& walker, Reference number)
{
    Representation form = walker.representationOf(number);
    return (form == Representation::Double || form == Representation::OptionalDouble) && !walker.isInvariantInInnermostLoop(number);
}

static bool convertOperandsOfBitOperatorOnce(Walker& walker, Opcode opcode, Origin origin, std::span<Reference> operands, const std::array<bool, 2>& areTakenByOperatorAlone)
{
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
        bool isMasked = opcode == Opcode::BitAnd && isMaskOfLowestBitsOfInt32(types.at(walker.typeOf(operands[1 - operandIndex])));
        if (isMasked && (!walker.isInLoop() || (areTakenByOperatorAlone[operandIndex] && isRawIterationNumber(walker, number))))
            continue;
        if (opcode != Opcode::BitNot && !type.isNumber() && walker.representationOf(number) == Representation::JSValue && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull))
            continue;
        hasConverted = true;
        if (walker.isInLoop() && walker.opensGuesses() && isBoxedNumberOrNullish(walker, number)) {
            operands[operandIndex] = buildBitsOfNumberOrNullish(walker, number, origin);
            continue;
        }
        if (!type.isNumber())
            number = buildOperatorInNumberForms(walker, Opcode::ToNumber, number, { }, origin);
        if (!number) {
            operands[operandIndex] = { };
            continue;
        }
        operands[operandIndex] = buildConversionInNumberOperators(walker, Opcode::ToInt32, number, origin);
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

    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands && numberOfOperands == 2; ++operandIndex) {
        const OperandInNumberOperators& other = operands[1 - operandIndex];
        PrimitiveKnownToFolding known = operands[operandIndex].known;
        if (!other.type.isNumber() || known.kind() != PrimitiveKnownToFolding::Kind::Number)
            continue;
        if (isIdentityElementOfBitOperator(opcode, operandIndex, toInt32(known.asNumber())))
            return bindInNumberOperators(walker, value, buildBitsOfNumberInNumberOperators(walker, opcode == Opcode::URShift ? Opcode::ToUint32 : Opcode::ToInt32, other.reference, origin));
    }

    std::array<Reference, 2> taken { operands[0].reference, operands[1].reference };
    std::span<Reference> takenByOperator = std::span(taken).first(numberOfOperands);
    std::array<bool, 2> areTakenByOperatorAlone { };
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
        areTakenByOperatorAlone[operandIndex] = walker.currentBodyIndex().onlyUsingValue(operands[operandIndex].written) == value.id();
    takesOtherOperands |= convertOperandsOfBitOperatorOnce(walker, opcode, origin, takenByOperator, areTakenByOperatorAlone);
    if (!takesOtherOperands)
        return false;
    return buildOnOperandsInNumberOperators(walker, value, takenByOperator);
}

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
        bool differsFromZeroAnyway = isEquality && other.type.isInteger() && !other.type.integers.contains(0);
        if (remainders[index]->mayBeNaN && !differsFromZeroAnyway)
            divisorsThatMayBeZero.append(remainders[index]->divisor);
    }

    Draft draft = walker.draftOf(comparison);
    draft.setOperand(0, compared[0]);
    draft.setOperand(1, compared[1]);
    return bindInNumberOperators(walker, comparison, buildComparisonWhereDivisorsAreNotZero(walker, WTF::move(draft), divisorsThatMayBeZero.span()));
}

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

static bool buildRelationWithNumberRoundedInFrontOfLoop(Walker& walker, const BodyValue& relation, const OperandsInNumberOperators& operands)
{
    if (!walker.isInLoop() || !walker.has(Mechanism::LoopInvariants))
        return false;
    for (uint32_t numberIndex = 0; numberIndex < 2; ++numberIndex) {
        const OperandInNumberOperators& number = operands[numberIndex];
        if (!operands[1 - numberIndex].type.isInteger() || !number.type.isNumber() || number.type.isInteger())
            continue;
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

static_assert(opcodesThatBuildNumberFormTakes == (opcodesOfNumberOperators));

static Reference buildProductWithoutNegativeZero(Walker& walker, const BodyValue& value, const OperandsInNumberOperators& operands)
{
    TypeArena& types = walker.types();
    if (!operands[0].type.isInteger() || !operands[1].type.isInteger())
        return { };
    Draft product(Opcode::MultiplyIntegers, walker.originOf(value));
    product.setMultiplicandOperand(operands[0].reference);
    product.setMultiplierOperand(operands[1].reference);
    const Type& products = types.at(walker.resultTypeOf(product));
    if (!products.isInteger() || !products.integers.isSubsetOf(Range::exact()) || products.integers.isSingleValue())
        return { };
    Range multiplicands = operands[0].type.integers;
    Range multipliers = operands[1].type.integers;
    bool mayBeNegativeZero = (multiplicands.minimum < 0 && multipliers.contains(0)) || (multipliers.minimum < 0 && multiplicands.contains(0));
    if (!mayBeNegativeZero || !isSignOfZeroIgnored(walker, value.id()))
        return { };
    walker.chooseConservativeFields(product);
    return walker.build(WTF::move(product));
}

bool buildNumberFormOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfNumberOperators.contains(opcode) || !hasMechanismsOfNumberForms(walker))
        return false;

    if (opcode == Opcode::LogicalNot)
        return bindWhatIsNegatedTwice(walker, value);

    if (allEqualities.contains(opcode) || allRelations.contains(opcode) || opcode == Opcode::ToBoolean) {
        bool somethingIsProven = false;
        for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(opcode) && !somethingIsProven; ++operandIndex)
            somethingIsProven |= !walker.hasUninformativeType(walker.referenceOf(value.operand(operandIndex)));
        if (!somethingIsProven)
            return false;
    }

    OperandsInNumberOperators operands;
    if (!readOperandsInNumberOperators(walker, value, operands))
        return false;

    if (arithmeticInNumberOperators.contains(opcode) && walker.has(Mechanism::Folding)) {
        if (Reference given = valueThatArithmeticGives(walker, opcode, operands)) {
            walker.noteValueThatFoldedArithmeticGives(given);
            return bindInNumberOperators(walker, value, given);
        }
    }
    if (opcode == Opcode::Add) {
        if (Reference middle = buildMiddleOfBounds(walker, value, operands))
            return bindInNumberOperators(walker, value, middle);
        if (Reference carry = buildWideProduct(walker, value))
            return bindInNumberOperators(walker, value, carry);
    }
    if (opcode == Opcode::BitAnd) {
        for (uint32_t operandIndex : { 0u, 1u }) {
            if (Reference bits = buildLowBitsOfWideProduct(walker, operands[operandIndex].reference, operands[1 - operandIndex].reference, originInFrontOfNumberOperator(walker, value)))
                return bindInNumberOperators(walker, value, bits);
        }
    }
    if (opcode == Opcode::Mul) {
        if (Reference product = buildProductWithoutNegativeZero(walker, value, operands))
            return bindInNumberOperators(walker, value, product);
    }
    if (opcode == Opcode::Pow && operands[0].type.isNumber() && isKnownIntegerInNumberOperators(operands[1].known, 2))
        return bindInNumberOperators(walker, value, buildOperatorInNumberForms(walker, Opcode::Mul, operands[0].reference, operands[0].reference, walker.originOf(value)));

    if (allBitwiseOperators.contains(opcode))
        return buildBitOperator(walker, value, operands);
    if (opcode == Opcode::Mod)
        return buildNumberRemainderWithItsTest(walker, value);

    InputsOfTypedOperation inputs(walker, nullptr);
    if (allEqualities.contains(opcode)) {
        if (buildComparisonOfWholeRemainders(walker, value, operands))
            return true;
        return buildComparisonByCodeUnit(walker, value, inputs);
    }

    if (allRelations.contains(opcode)) {
        if (buildRelationWithNumberRoundedInFrontOfLoop(walker, value, operands))
            return true;
        if (buildComparisonOfWholeRemainders(walker, value, operands))
            return true;
        return buildComparisonByCodeUnit(walker, value, inputs);
    }

    if (opcode == Opcode::ToBoolean) {
        std::optional<WholeRemainder> remainder = findWholeRemainder(walker, operands[0].written, inputs);
        if (!remainder)
            return false;
        Reference integer = buildZeroWhereWholeRemainderIsZero(walker, originInFrontOfNumberOperator(walker, value), *remainder);
        return buildOnOperandsInNumberOperators(walker, value, singleElementSpan(integer));
    }

    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
