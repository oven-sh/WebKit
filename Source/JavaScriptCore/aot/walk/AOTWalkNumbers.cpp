/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOperatorForms.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTWalkNumberGuesses.h"
#include <limits>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
OperatorForm operatorFormFor(Walker&, Opcode, const BodyValue&, const InputsOfTypedOperation&);
Reference checkIntegerBeforeLoop(Walker&, Reference value, TypeID tested, Origin, StatisticID counted);

Representation representationOfNumber(const Walker& walker, TypeID type)
{
    if (!type || !walker.has(Mechanism::UnboxedNumbers))
        return Representation::JSValue;
    Representation natural = representationOf(TypeReader(walker.types()), type);
    return numberRepresentations.contains(natural) ? natural : Representation::JSValue;
}

Reference boxNumberIfNeeded(Walker& walker, Reference number, Origin origin)
{
    if (!number.isValue())
        return number;
    Representation held = walker.representationOf(number);
    TypeID type = walker.typeOf(number);
    if (!numberRepresentations.contains(held) || representationOfNumber(walker, type) != Representation::JSValue)
        return number;

    std::optional<ConversionPath> path = findConversion(walker.types(), type, held, Representation::JSValue);
    if (!path)
        return number;
    for (ConversionStep step : *path) {
        Draft conversion(step.opcode, origin);
        conversion.setOperand(0, number);
        conversion.representation = step.to;
        number = walker.build(WTF::move(conversion));
    }
    return number;
}

bool isTakenAsItIsByFormNumbers(const Type& type)
{
    return type.isNumberOrNullish() || type.isUndefinedOrNull();
}

static bool isClosedFormOfOperatorOnNumbers(Opcode opcode, OperatorForm form)
{
    bool takesAnyValue = form == OperatorForm::Generic || form == OperatorForm::NumberAndValue || form == OperatorForm::MayBeString;
    return takesAnyValue && !openUnderOf(opcode).contains(stateOf(form));
}

static Reference convertedForFormNumbers(Walker& walker, Reference operand, Origin origin)
{
    Draft conversion(Opcode::ToNumber, origin);
    conversion.setValueOperand(operand);
    walker.chooseConservativeFields(conversion);
    conversion.setForm(operatorFormFor(walker, conversion));
    return walker.build(WTF::move(conversion));
}

void prepareOperandsForFormNumbers(Walker& walker, Draft& draft)
{
    Origin origin = draft.origin();
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands(); ++operandIndex) {
        Reference operand = draft.operand(operandIndex);
        const Type& type = walker.types().at(walker.typeOf(operand));
        bool isConvertedBeforeLoop = (type.primitives & Primitive::Number) && !type.isNumber() && operand.isValue() && walker.has(Mechanism::LoopInvariants) && walker.isInvariantInInnermostLoop(operand);
        if (isConvertedBeforeLoop || !isTakenAsItIsByFormNumbers(type))
            draft.setOperand(operandIndex, convertedForFormNumbers(walker, operand, origin));
    }
}

static Reference buildRemainderByReciprocalMadeInFrontOfLoop(Walker& walker, const Draft& remainder)
{
    constexpr Range dividendsOfReciprocal { 0, std::numeric_limits<uint32_t>::max() };
    constexpr Range divisorsOfReciprocal { 1, std::numeric_limits<int32_t>::max() };
    std::optional<uint32_t> loop = walker.has(Mechanism::LoopInvariants) ? walker.innermostOpenLoop() : std::nullopt;
    Reference dividend = remainder.operand(OperandIndex::Mod::left);
    Reference divisor = remainder.operand(OperandIndex::Mod::right);
    if (!loop || !divisor.isValue() || !walker.standsInFrontOfOpenLoop(*loop, divisor) || walker.standsInFrontOfOpenLoop(*loop, dividend))
        return { };
    Type dividends = walker.types().at(walker.typeOf(dividend));
    Type divisors = walker.types().at(walker.typeOf(divisor));
    if (!dividends.isInteger() || (!dividends.integers.isSubsetOf(dividendsOfReciprocal) && !walker.shouldAssumeUInt32Dividends()) || !divisors.isInteger() || !divisors.integers.isSubsetOf(divisorsOfReciprocal) || divisors.integers.isSingleValue())
        return { };
    static constexpr uint32_t maxDividingIterations = 4;
    if (std::optional<uint32_t> turns = walker.constantTripCountOf(*loop); turns && *turns <= maxDividingIterations)
        return { };

    Draft inFront(Opcode::ReciprocalOfDivisor, remainder.origin());
    walker.chooseConservativeFields(inFront);
    inFront.setDivisorOperand(divisor);
    Reference reciprocal = walker.build(WTF::move(inFront));
    if (!reciprocal || !walker.standsInFrontOfOpenLoop(*loop, reciprocal))
        return { };

    Draft byReciprocal(Opcode::RemainderByReciprocal, remainder.origin());
    walker.chooseConservativeFields(byReciprocal);
    byReciprocal.setDividendOperand(dividend);
    byReciprocal.setDivisorOperand(divisor);
    byReciprocal.setReciprocalOperand(reciprocal);
    Reference built = walker.build(WTF::move(byReciprocal));
    if (built)
        walker.count(StatisticID::RemaindersByHoistedReciprocal);
    return built;
}

static Reference buildRemainderByWholeDivisor(Walker& walker, const Draft& remainder)
{
    std::optional<uint32_t> loop = walker.has(Mechanism::LoopInvariants) ? walker.innermostOpenLoop() : std::nullopt;
    Reference dividend = remainder.operand(OperandIndex::Mod::left);
    Reference divisor = remainder.operand(OperandIndex::Mod::right);
    if (!loop || !divisor.isValue() || !walker.standsInFrontOfOpenLoop(*loop, divisor) || walker.standsInFrontOfOpenLoop(*loop, dividend))
        return { };
    const Type& typeOfDivisor = walker.types().at(walker.typeOf(divisor));
    if (!walker.types().at(walker.typeOf(dividend)).isNumber() || !typeOfDivisor.isNumber() || typeOfDivisor.isInteger())
        return { };

    Draft inFront(Opcode::WholeDivisor, remainder.origin());
    walker.chooseConservativeFields(inFront);
    inFront.setDivisorOperand(divisor);
    Reference wholeDivisor = walker.build(WTF::move(inFront));
    if (!wholeDivisor || !walker.standsInFrontOfOpenLoop(*loop, wholeDivisor))
        return { };

    Draft byWholeDivisor(Opcode::RemainderByWholeDivisor, remainder.origin());
    walker.chooseConservativeFields(byWholeDivisor);
    byWholeDivisor.setDividendOperand(dividend);
    byWholeDivisor.setDivisorOperand(divisor);
    byWholeDivisor.setWholeDivisorOperand(wholeDivisor);
    return walker.build(WTF::move(byWholeDivisor));
}

static bool hoistOperandCheck(Walker& walker, Draft& draft)
{
    static constexpr OpcodeSet operators { Opcode::Add, Opcode::Sub, Opcode::Mul };
    if (!operators.contains(draft.opcode) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    TypeArena& types = walker.types();
    for (uint32_t side : { 0u, 1u }) {
        Reference operand = draft.operand(side);
        Reference other = draft.operand(1 - side);
        if (!operand || !other || !operand.isValue() || !types.at(walker.typeOf(other)).isInteger())
            continue;
        Type type = types.at(walker.typeOf(operand));
        if (!(type.primitives & Primitive::Integer) || !type.hasOnlyPrimitives(Primitive::Integer | Primitive::Undefined | Primitive::Absent) || type.isInteger())
            continue;
        if (!walker.isInvariantInSpecializedVersion(operand, Walker::IterationPurity::KnownEffectsOnly))
            return false;
        Reference integer = checkIntegerBeforeLoop(walker, operand, types.integer(type.integers), draft.origin(), StatisticID::InvariantNumberGuardsHoisted);
        if (!integer)
            return false;
        if (!walker.state().checkedNameOf(operand))
            walker.state().noteCheckedName(operand, integer);
        draft.setOperand(side, integer);
        return true;
    }
    return false;
}

bool buildOperatorOnNumbers(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    OperatorForm form = operatorFormFor(walker, value.opcode(), value, inputs);
    if (isClosedFormOfOperatorOnNumbers(value.opcode(), form))
        return false;
    if (inputs.areOnlyTypes())
        return true;

    Draft draft = walker.draftOf(value);
    if (form == OperatorForm::Numbers && hoistOperandCheck(walker, draft))
        form = operatorFormFor(walker, draft);
    if (walker.hasFailed())
        return true;
    if (form == OperatorForm::Numbers) {
        prepareOperandsForFormNumbers(walker, draft);
        form = operatorFormFor(walker, draft);
    }
    if (form == OperatorForm::CheckedIntegers)
        form = operatorFormFor(walker, draft);
    walker.chooseConservativeFields(draft);
    draft.setForm(form);
    if (sumStaysBoxed(walker, value, draft))
        draft.representation = Representation::JSValue;
    Origin origin = draft.origin();
    Reference remainder;
    if (value.opcode() == Opcode::Mod && form == OperatorForm::Integers)
        remainder = buildRemainderByReciprocalMadeInFrontOfLoop(walker, draft);
    else if (value.opcode() == Opcode::Mod && form == OperatorForm::Numbers)
        remainder = buildRemainderByWholeDivisor(walker, draft);
    walker.bind(value.id(), boxNumberIfNeeded(walker, remainder ? remainder : walker.build(WTF::move(draft)), origin));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
