/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

CharacterView StringEmitter::viewOfRange(const ValueContext& value)
{
    CharacterView view = viewOf(value, value->stringOperand());
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    Integer units = zeroExtended(start);
    Pointer characters = select(view.is8Bit, offsetBy(view.characters, units, 0), offsetBy(view.characters, units, 1));
    return { characters, sub(end, start), view.is8Bit, view.owner };
}

void StringEmitter::generateStringRangeEqualsLiteral(const ValueContext& value)
{
    StringID literal = value->literal();
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    unsigned length = textOf(literal).length();
    if (!length) {
        defineTest(value, equal(start, end));
        return;
    }
    if (!canCompareInline(literal)) {
        fail(value, InternalErrorKind::PayloadOutOfRange);
        defineTest(value, boolean(false));
        return;
    }

    defineTestByBranching(value, [&](BranchTargets targets) {
        continueIf(equal(sub(end, start), word32(length)), targets.notTaken);
        branchOnTextAt(viewOf(value, value->stringOperand()), start, literal, targets);
    });
}

void StringEmitter::generateStringRangeToNumber(const ValueContext& value)
{
    CharacterView view = viewOfRange(value);
    Number number = selectByWidth<Number>(view.is8Bit, [&](bool is8Bit) {
        return callLeaf<Number>(is8Bit ? RuntimeOperation::StringToNumber8 : RuntimeOperation::StringToNumber16, { view.characters, view.length });
    }, Machine::EdgeClass::Normal);
    ensureStillAliveHere(view.owner);
    define(value, emitter<ConversionEmitter>().inRepresentation(value, number, typeOf(value.id()), value.representation()));
}

void StringEmitter::generateStringRangeParseInt(const ValueContext& value)
{
    ValueID radixOperand = value->radixOperand();
    Word32 radix = radixOperand ? word32Of(radixOperand) : word32(0);
    CharacterView view = viewOfRange(value);
    Number number = selectByWidth<Number>(view.is8Bit, [&](bool is8Bit) {
        return callLeaf<Number>(is8Bit ? RuntimeOperation::ParseInt8 : RuntimeOperation::ParseInt16, { view.characters, view.length, radix });
    }, Machine::EdgeClass::Normal);
    ensureStillAliveHere(view.owner);
    define(value, emitter<ConversionEmitter>().inRepresentation(value, number, typeOf(value.id()), value.representation()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
