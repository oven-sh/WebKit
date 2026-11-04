/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The doors of the forms that go by the order of keys, by copies of objects and by what lies in a slot beside a value: one link of the chain and one term of the row of guesses. Each part asks for its own mechanisms.
// Folding, a value compared with itself: `a === a` is true of whatever is not NaN. It keeps nothing, so neither a suspension nor a throw ends anything. Withheld: the comparison is built as ever.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet operationsOfLinkInWalkCopies {
    Opcode::StrictEq, Opcode::CopyObject, Opcode::Call,
    Opcode::PropGet, Opcode::PropSet, Opcode::PropHas, Opcode::PropDelete,
    Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete
};

extern const OpcodeSet opcodesThatBuildCopyOrKeyOrderFormTakes;
constinit const OpcodeSet opcodesThatBuildCopyOrKeyOrderFormTakes = operationsOfLinkInWalkCopies;

bool buildCopyOrKeyOrderFormOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!operationsOfLinkInWalkCopies.contains(value.opcode()))
        return false;
    return foldComparisonOfValueWithItself(walker, value)
        || foldLookupPastOwnNames(walker, value)
        || buildAccessThroughProxyInSight(walker, value)
        || assignSourcesFieldByField(walker, value)
        || buildCopyThatMakesObject(walker, value);
}

bool openGuessOfKeyOrdersOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    return callAccessorInSlot(walker, value) || accessStaticFieldInVariable(walker, value);
}

// Behind every record that hands on the value it is given, in another form or under a name that says more of it.
static ValueID valueBehindNamesInWalkCopies(const Code& code, ValueID value)
{
    while (!code.isConstant(value) && opcodesThatGiveSameCell.contains(code.at(value).opcode))
        value = code.at(value).operand(0);
    return value;
}

bool foldComparisonOfValueWithItself(Walker& walker, const BodyValue& comparison)
{
    if (comparison.opcode() != Opcode::StrictEq || !walker.has(Mechanism::Folding))
        return false;
    auto operation = comparison.as<Opcode::StrictEq>();
    Reference left = walker.referenceOf(operation.leftOperand());
    Reference right = walker.referenceOf(operation.rightOperand());
    if (!left || !right || !left.isRecord() || !right.isRecord())
        return false;
    const Code& code = walker.code();
    if (valueBehindNamesInWalkCopies(code, left.id()) != valueBehindNamesInWalkCopies(code, right.id()))
        return false;
    // Only NaN differs from itself. Either name may say that the value is none.
    Type typeOfLeft = walker.types().at(walker.typeOf(left));
    Type typeOfRight = walker.types().at(walker.typeOf(right));
    if (typeOfLeft.isNever() || typeOfRight.isNever() || !typeOfLeft.isJavaScriptValue() || (typeOfLeft.mayBeNaN() && typeOfRight.mayBeNaN()))
        return false;

    Draft answer = walker.draftOf(comparison);
    if (walker.hasFailed())
        return true;
    answer.become(Opcode::Boolean);
    answer.setBoolean(true);
    walker.bind(comparison.id(), walker.constant(WTF::move(answer)));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
