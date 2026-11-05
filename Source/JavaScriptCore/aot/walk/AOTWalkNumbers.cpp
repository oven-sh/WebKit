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
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
OperatorForm formAllowedForTypedOperator(Walker&, Opcode, const BodyValue&, const InputsOfTypedOperation&);

Representation representationOfNumber(const Walker& walker, TypeID type)
{
    if (!type || !walker.has(Mechanism::UnboxedNumbers))
        return Representation::JSValue;
    Representation natural = representationOf(TypeReader(walker.types()), type);
    return numberRepresentations.contains(natural) ? natural : Representation::JSValue;
}

Reference boxedUnlessNumbersAreUnboxed(Walker& walker, Reference number, Origin origin)
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
    conversion.setForm(formAllowedForTypedOperator(walker, conversion));
    return walker.build(WTF::move(conversion));
}

void prepareOperandsForFormNumbers(Walker& walker, Draft& draft)
{
    Origin origin = draft.origin();
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands(); ++operandIndex) {
        Reference operand = draft.operand(operandIndex);
        if (!isTakenAsItIsByFormNumbers(walker.types().at(walker.typeOf(operand))))
            draft.setOperand(operandIndex, convertedForFormNumbers(walker, operand, origin));
    }
}

bool buildOperatorOnNumbers(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    OperatorForm form = formAllowedForTypedOperator(walker, value.opcode(), value, inputs);
    if (isClosedFormOfOperatorOnNumbers(value.opcode(), form))
        return false;
    if (inputs.areOnlyTypes())
        return true;

    Draft draft = walker.draftOf(value);
    if (form == OperatorForm::Numbers) {
        prepareOperandsForFormNumbers(walker, draft);
        form = formAllowedForTypedOperator(walker, draft);
    }
    if (form == OperatorForm::CheckedIntegers)
        form = formAllowedForTypedOperator(walker, draft);
    walker.chooseConservativeFields(draft);
    draft.setForm(form);
    if (sumStaysBoxed(walker, value, draft))
        draft.representation = Representation::JSValue;
    Origin origin = draft.origin();
    walker.bind(value.id(), boxedUnlessNumbersAreUnboxed(walker, walker.build(WTF::move(draft)), origin));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
