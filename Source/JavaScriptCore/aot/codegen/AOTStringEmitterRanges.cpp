/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTStubsCollections.h"

namespace JSC { namespace AOT { namespace Typed {

void StringEmitter::holdRangeInside(const ValueContext& value, Word32 start, Word32 end)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    if (!elements.checksBounds())
        return;
    if (start.value() != end.value())
        elements.stopUnlessInside(value, belowOrEqual(start, end));
    elements.stopUnlessInside(value, belowOrEqual(end, lengthOf(value->stringOperand())));
}

CharacterView StringEmitter::viewOfRange(const ValueContext& value)
{
    CharacterView view = viewOf(value, value->stringOperand());
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    holdRangeInside(value, start, end);
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
        holdRangeInside(value, start, end);
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

void StringEmitter::generateFindStringRangeInMap(const ValueContext& value)
{
    CellPointer map = in<CellPointer>(value->mapOperand());
    CharacterView view = viewOf(value, value->stringOperand());
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    holdRangeInside(value, start, end);
    Word32 length = sub(end, start);
    Integer place = selectByWidth<Integer>(view.is8Bit, [&](bool is8Bit) {
        Pointer characters = offsetBy(view.characters, zeroExtended(start), is8Bit ? 0 : 1);
        if (is8Bit)
            return Integer(callStub(value, findCharactersInMap(), { map.value(), characters.value(), length.value() }).first);
        return callLeaf<Integer>(RuntimeOperation::FindCharacters16InMap, { map, characters, length });
    });
    ensureStillAliveHere(view.owner);
    define(value, place);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
