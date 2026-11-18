/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

Reference buildCharCode(Walker&, const CallByOneRow&)
{
    return { };
}

Reference codeUnitBesideCharCode(Walker& walker, Reference number)
{
    TypeID id = number.isValue() ? walker.typeOf(number) : TypeID();
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (!type.isNumber() || type.isInteger())
        return { };
    return codeUnitOfCharCodeInSight(walker, number);
}

Reference codePointBesideCharacter(Walker& walker, Reference character)
{
    TypeID id = character.isValue() ? walker.typeOf(character) : TypeID();
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (type.isNever() || !type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined))
        return { };
    return codePointOfCharacterInSight(walker, character);
}

static bool isNeverNegativeInCodeUnits(Walker& walker, Reference integer)
{
    TypeID id = walker.typeOf(integer);
    if (!id)
        return false;
    Type type = walker.types().at(id);
    return type.isInteger() && type.integers.minimum >= 0;
}

Reference integerThatCharCodeIsKnownToBe(Walker& walker, Reference number)
{
    Reference codeUnit = codeUnitBesideCharCode(walker, number);
    return codeUnit && isNeverNegativeInCodeUnits(walker, codeUnit) ? codeUnit : Reference();
}

static ValueID valueBehindNamesOfBodyInCodeUnits(const BodyIndex& index, ValueID value)
{
    while (value) {
        value = index.stripCasts(value);
        ValueID tested = value ? index.valueNarrowedInto(value) : ValueID();
        if (!tested)
            break;
        value = tested;
    }
    return value;
}

static bool standsBehindTestThatNaNFails(const BodyIndex& index, const BodyValue& user, ValueID number)
{
    std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(index.blockOf(user.id()));
    if (!edge)
        return false;
    BodyTerminator branch = index.terminator(edge->from);
    if (!branch.isConditionalBranch())
        return false;

    bool holds = !edge->targetIndex;
    ValueID condition = index.stripCasts(branch.operand(0));
    while (condition && !index.isParameter(condition) && (index.opcodeOf(condition) == Opcode::LogicalNot || index.opcodeOf(condition) == Opcode::ToBoolean)) {
        if (index.opcodeOf(condition) == Opcode::LogicalNot)
            holds = !holds;
        condition = index.stripCasts(index.value(condition).valueOperand());
    }
    if (!holds || !condition || index.isParameter(condition))
        return false;
    Opcode opcode = index.opcodeOf(condition);
    if (!allRelations.contains(opcode) && !allEqualities.contains(opcode))
        return false;

    BodyValue test = index.value(condition);
    ValueID named = valueBehindNamesOfBodyInCodeUnits(index, number);
    return valueBehindNamesOfBodyInCodeUnits(index, test.operand(0)) == named || valueBehindNamesOfBodyInCodeUnits(index, test.operand(1)) == named;
}

Reference integerThatCharCodeIsKnownToBe(Walker& walker, const BodyValue& user, ValueID number)
{
    Reference codeUnit = number ? codeUnitBesideCharCode(walker, walker.referenceOf(number)) : Reference();
    if (!codeUnit)
        return { };
    if (isNeverNegativeInCodeUnits(walker, codeUnit) || standsBehindTestThatNaNFails(walker.currentBodyIndex(), user, number))
        return codeUnit;
    return { };
}

bool buildComparisonByCodeUnit(Walker& walker, const BodyValue& comparison, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = comparison.opcode();
    bool isEquality = allEqualities.contains(opcode);
    if (inputs.areOnlyTypes() || (!isEquality && !allRelations.contains(opcode)))
        return false;

    for (uint32_t numberIndex = 0; numberIndex < 2; ++numberIndex) {
        Reference other = walker.referenceOf(comparison.operand(1 - numberIndex));
        TypeID typeOfOther = other && other.isRecord() ? walker.typeOf(other) : TypeID();
        if (!typeOfOther || !walker.types().at(typeOfOther).isInt32())
            continue;
        Reference codeUnit = codeUnitBesideCharCode(walker, walker.referenceOf(comparison.operand(numberIndex)));
        if (!codeUnit)
            continue;

        Range others = walker.types().at(typeOfOther).integers;
        bool asksForGreaterOnLeft = opcode == Opcode::Greater || opcode == Opcode::GreaterEq;
        bool numberIsOnLeft = !numberIndex;
        bool charCodeIsLesserOperand = !isEquality && asksForGreaterOnLeft != numberIsOnLeft;
        if (!charCodeIsLesserOperand && (isEquality ? others.contains(-1) : others.minimum < 0))
            return false;

        Draft draft = walker.draftOf(comparison);
        Origin origin = draft.origin();
        origin.isTailCall = false;
        if (charCodeIsLesserOperand && !isNeverNegativeInCodeUnits(walker, codeUnit))
            codeUnit = buildOperatorInNumberForms(walker, Opcode::URShift, codeUnit, integerInNumberForms(walker, 0), origin);
        draft.setOperand(numberIndex, codeUnit);
        draft.setOperand(1 - numberIndex, other);
        if (Reference answer = buildInAllowedFormInNumberForms(walker, WTF::move(draft)))
            walker.bind(comparison.id(), answer);
        return true;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
