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
#include "JSString.h"

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

Condition StringEmitter::isLongRope(ValueID string)
{
    if (m_code.isConstant(string) || typeOf(string).isOpaque(OpaqueKind::FlatString) || generator().findCarriedSubstring(string))
        return boolean(false);
    CellPointer cell = in<CellPointer>(string);
    Pointer fiber = loadField<Pointer>(cell, m_heaps.JSString_value);
    return selectByBranch<Condition>(Condition(m_out.testNonZeroPtr(fiber.value(), JSString::isRopeInPointer)), [&] {
        Word32 length = loadField<Word32>(cell, m_heaps.JSRopeString_length);
        return logicalAnd(hasNoBits(fiber, JSRopeString::isSubstringInPointer), aboveOrEqual(length, word32(JSString::minLengthForRopeWalk)));
    }, [&] {
        return boolean(false);
    }, Machine::EdgeClass::Unlikely);
}

// The range is cut out of a long rope, as slice() would have, which resolves the rope only where the range lies in several of its parts.
CharacterView StringEmitter::viewOfRange(const OperationContext& context, ValueID string, CodeUnitRange range)
{
    auto rangeIn = [&](const CharacterView& view, Word32 start) -> CharacterView {
        Integer units = zeroExtended(start);
        Pointer characters = select(view.is8Bit, offsetBy(view.characters, units, 0), offsetBy(view.characters, units, 1));
        return { characters, sub(range.end, range.start), view.is8Bit, view.owner };
    };
    Condition isCut = isLongRope(string);
    if (isCut.value()->isInt(0))
        return rangeIn(viewOf(context, string), range.start);
    CellPointer whole = in<CellPointer>(string);
    CellPointer holder = selectByBranch<CellPointer>(isCut, [&] {
        return substring(context, whole, range);
    }, [&] {
        return whole;
    }, Machine::EdgeClass::Unlikely);
    return rangeIn(viewOfString(context, holder), select(isCut, word32(0), range.start));
}

CharacterView StringEmitter::viewOfRange(const ValueContext& value)
{
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    holdRangeInside(value, start, end);
    return viewOfRange(value, value->stringOperand(), { start, end });
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
        // A group that took no part in a match has no start. The bytecode interpreter leaves its end as it was.
        if (typeOf(value->startOperand()).integers.minimum < 0)
            continueIf(greaterThanOrEqual(start, word32(0)), targets.notTaken);
        holdRangeInside(value, start, end);
        branchOnTextAt(viewOfRange(value, value->stringOperand(), { start, end }), word32(0), literal, targets);
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
    CharacterView view = viewOfRange(value);
    Integer place = selectByWidth<Integer>(view.is8Bit, [&](bool is8Bit) {
        if (is8Bit)
            return Integer(callStub(value, findCharactersInMap(), { map.value(), view.characters.value(), view.length.value() }).first);
        return callLeaf<Integer>(RuntimeOperation::FindCharacters16InMap, { map, view.characters, view.length });
    });
    ensureStillAliveHere(view.owner);
    define(value, place);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
