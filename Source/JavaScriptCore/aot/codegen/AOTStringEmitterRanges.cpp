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

bool StringEmitter::isNoRopeByItsForm(ValueID string)
{
    return m_code.isConstant(string) || typeOf(string).isOpaque(OpaqueKind::FlatString) || generator().findCarriedSubstring(string);
}

Condition StringEmitter::isLongRope(ValueID string)
{
    if (isNoRopeByItsForm(string))
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

// Has the length and the places of the whole string, but only the range may be read, and only if it lies inside the string: see RopeCharactersAroundRange.
CharacterView StringEmitter::viewForReadOfRange(const OperationContext& context, ValueID string, const RangeInString& range)
{
    return isNoRopeByItsForm(string) ? viewOf(context, string) : viewOfString(context, in<CellPointer>(string), &range);
}

CharacterView StringEmitter::viewForReadOfRange(const ValueContext& value, CodeUnitRange range)
{
    holdRangeInside(value, range.start, range.end);
    return viewForReadOfRange(value, value->stringOperand(), scopedLambda<CodeUnitRange(Word32)>([&](Word32) { return range; }));
}

CharacterView StringEmitter::viewOfRange(const ValueContext& value)
{
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    CharacterView view = viewForReadOfRange(value, { start, end });
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
        // A group that took no part in a match has no start. The bytecode interpreter leaves its end as it was.
        if (typeOf(value->startOperand()).integers.minimum < 0)
            continueIf(greaterThanOrEqual(start, word32(0)), targets.notTaken);
        branchOnTextAt(viewForReadOfRange(value, { start, end }), start, literal, targets);
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
