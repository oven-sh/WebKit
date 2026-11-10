/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

bool buildCopyOrKeyOrderFormOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::StrictEq:
        return foldComparisonOfValueWithItself(walker, value);
    case Opcode::CopyObject:
        if (!buildCopyThatMakesObject(walker, value))
            return false;
        walker.count(StatisticID::OwnKeyWalksExpanded);
        return true;
    case Opcode::Call:
        return assignSourcesFieldByField(walker, value);
    default:
        return false;
    }
}

bool openGuessOfKeyOrdersOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    return callAccessorInSlot(walker, value) || accessStaticFieldInVariable(walker, value);
}

static ALWAYS_INLINE ValueID valueBehindNamesInWalkCopies(const Code& code, ValueID value)
{
    while (!code.isConstant(value) && cellPreservingOpcodes.contains(code.at(value).opcode))
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
