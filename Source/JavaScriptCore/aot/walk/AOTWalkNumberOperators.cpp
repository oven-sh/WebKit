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

static Reference buildSimplerOperandOfBitOperator(Walker& walker, Origin origin, ValueID written)
{
    InputsOfTypedOperation inputs(walker, nullptr);
    if (std::optional<DivisionAsWritten> quotient = findQuotientOfIntegers(walker, written, inputs))
        return buildDivisionOfIntegers(walker, origin, walker.referenceOf(quotient->dividend), walker.referenceOf(quotient->divisor), QuotientRounding::ToInt32);
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

static bool convertOperandsOfBitOperatorOnce(Walker& walker, Opcode opcode, Origin origin, std::span<Reference> operands)
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
        if (!walker.isInLoop() && opcode == Opcode::BitAnd && isMaskOfLowestBitsOfInt32(types.at(walker.typeOf(operands[1 - operandIndex]))))
            continue;
        hasConverted = true;
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
    takesOtherOperands |= convertOperandsOfBitOperatorOnce(walker, opcode, origin, takenByOperator);
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
