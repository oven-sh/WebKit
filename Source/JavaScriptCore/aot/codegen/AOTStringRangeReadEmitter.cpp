/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringRangeReadEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "PureNaN.h"

namespace JSC { namespace AOT { namespace Typed {

static void failAndDefineZero(CodeGenerator& generator, const ValueContext& value)
{
    generator.fail(value, InternalErrorKind::OpcodeWithoutCode);
    generator.define(value, generator.emitter<ConstantEmitter>().zeroOf(value.representation()));
}

static Operand readIfInRange(Output& out, Condition isInRange, Representation representation, NOESCAPE const Invocable<Operand()> auto& readInside, NOESCAPE const Invocable<Operand()> auto& readOutside)
{
    LBasicBlock inside = out.newBlock();
    LBasicBlock outside = out.newBlock();
    OperandJoin join(out, representation);
    out.branch(isInRange.value(), normalEdge(inside), unlikelyEdge(outside));
    out.appendTo(inside);
    join.add(readInside());
    out.appendTo(outside);
    join.add(readOutside());
    return join.finish();
}

StringRangeReadEmitter::StringRangeReadEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

CharacterView StringRangeReadEmitter::viewOfRange(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    CharacterView view = strings.viewOf(value, value->stringOperand());
    ValueID start = value->startOperand();
    ValueID end = value->endOperand();
    if (!start && !end)
        return view;
    return strings.subview(view, { start ? strings.word32Of(start) : word32(0), end ? strings.word32Of(end) : view.length });
}

auto StringRangeReadEmitter::openIndexedRead(const ValueContext& value) -> IndexedRead
{
    StringEmitter& strings = emitter<StringEmitter>();
    CharacterView view = viewOfRange(value);
    // A negative position is above every length when it is read as unsigned.
    Operand position = get(value->positionOperand());
    if (position.is<Word32>())
        return { view, position.as<Word32>(), below(position.as<Word32>(), view.length) };
    Integer truncated = position.is<Number>() ? strings.truncateToIndex(position.as<Number>()) : integerOf(value->positionOperand());
    return { view, low32(truncated), below(truncated, zeroExtended(view.length)) };
}

Number StringRangeReadEmitter::toNumber(const CharacterView& view)
{
    Number result = selectByBranch<Number>(view.is8Bit, [&] {
        return callLeaf<Number>(RuntimeOperation::StringToNumber8, { view.characters, view.length });
    }, [&] {
        return callLeaf<Number>(RuntimeOperation::StringToNumber16, { view.characters, view.length });
    });
    ensureStillAliveHere(view.owner);
    return result;
}

Number StringRangeReadEmitter::parseInt(const CharacterView& view, Word32 radix)
{
    Number result = selectByBranch<Number>(view.is8Bit, [&] {
        return callLeaf<Number>(RuntimeOperation::ParseInt8, { view.characters, view.length, radix });
    }, [&] {
        return callLeaf<Number>(RuntimeOperation::ParseInt16, { view.characters, view.length, radix });
    });
    ensureStillAliveHere(view.owner);
    return result;
}

void StringRangeReadEmitter::generateStringRangeCodeUnitOrNaN(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    IndexedRead read = openIndexedRead(value);
    if (value.representation() == Representation::Int32) {
        define(value, strings.codeUnitAt(read.view, read.index));
        return;
    }
    if (value.representation() != Representation::Double) {
        failAndDefineZero(generator(), value);
        return;
    }
    define(value, readIfInRange(m_out, read.isInRange, Representation::Double, [&] {
        return PrimitiveEmitter::toNumber(strings.codeUnitAt(read.view, read.index));
    }, [&] {
        return number(PNaN);
    }));
}

void StringRangeReadEmitter::generateStringRangeCodeUnitOrMinusOne(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    if (value.representation() != Representation::Int32) {
        failAndDefineZero(generator(), value);
        return;
    }
    IndexedRead read = openIndexedRead(value);
    define(value, readIfInRange(m_out, read.isInRange, Representation::Int32, [&] {
        return strings.codeUnitAt(read.view, read.index);
    }, [&] {
        return word32(-1);
    }));
}

void StringRangeReadEmitter::generateStringRangeCodePointOrUndefined(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    IndexedRead read = openIndexedRead(value);
    if (value.representation() == Representation::Int32) {
        define(value, strings.codePointAt(read.view, read.index));
        return;
    }
    if (value.representation() != Representation::OptionalInt64) {
        failAndDefineZero(generator(), value);
        return;
    }
    define(value, readIfInRange(m_out, read.isInRange, Representation::OptionalInt64, [&] {
        return conversions.fromBits(zeroExtended(strings.codePointAt(read.view, read.index)), Representation::OptionalInt64);
    }, [&] {
        uint64_t marker = ConversionEmitter::markerBits(Primitive::Undefined, Representation::OptionalInt64);
        return conversions.fromBits(integer(static_cast<int64_t>(marker)), Representation::OptionalInt64);
    }));
}

void StringRangeReadEmitter::generateStringRangeCharacterOrEmpty(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    if (value.representation() != Representation::Cell) {
        failAndDefineZero(generator(), value);
        return;
    }
    IndexedRead read = openIndexedRead(value);
    define(value, readIfInRange(m_out, read.isInRange, Representation::Cell, [&] {
        Word32 codeUnit = strings.codeUnitAt(read.view, read.index);
        return strings.stringFromCodeUnit(value, codeUnit);
    }, [&] {
        return emptyString();
    }));
}

void StringRangeReadEmitter::generateStringRangeCharacterOrUndefined(const ValueContext& value)
{
    IndexedRead read = openIndexedRead(value);
    ValueID position = value->positionOperand();
    // "abc"[1.5] and "abc"[NaN] are undefined, while "abc".charAt(1.5) is "b".
    if (!typeOf(position).isInteger())
        read.isInRange = logicalAnd(read.isInRange, equal(PrimitiveEmitter::toNumber(read.index), in<Number>(position)));
    defineCharacter(value, read, undefined());
}

void StringRangeReadEmitter::defineCharacter(const ValueContext& value, const IndexedRead& read, Boxed whenOutOfRange)
{
    StringEmitter& strings = emitter<StringEmitter>();
    auto readCharacter = [&] {
        Word32 codeUnit = strings.codeUnitAt(read.view, read.index);
        return strings.stringFromCodeUnit(value, codeUnit);
    };
    if (value.representation() == Representation::Cell) {
        define(value, readCharacter());
        return;
    }
    if (value.representation() != Representation::JSValue) {
        failAndDefineZero(generator(), value);
        return;
    }
    define(value, readIfInRange(m_out, read.isInRange, Representation::JSValue, [&] {
        return boxCell(readCharacter());
    }, [&] {
        return whenOutOfRange;
    }));
}

void StringRangeReadEmitter::generateStringRangeEqualsLiteral(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    CharacterView view = viewOfRange(value);
    unsigned length = strings.textOf(value->literal()).length();
    if (!length) {
        ensureStillAliveHere(view.owner);
        defineTest(value, isZero(view.length));
        return;
    }

    LBasicBlock hasSameLength = newBlock();
    LBasicBlock hasOtherLength = newBlock();
    Join<Condition> join(m_out);
    m_out.branch(equal(view.length, word32(length)).value(), normalEdge(hasSameLength), normalEdge(hasOtherLength));
    m_out.appendTo(hasSameLength);
    join.add(strings.hasTextAt(view, word32(0), value->literal()));
    m_out.appendTo(hasOtherLength);
    join.add(boolean(false));
    defineTest(value, join.finish());
}

void StringRangeReadEmitter::generateStringRangeToNumber(const ValueContext& value)
{
    if (value.representation() != Representation::Double) {
        failAndDefineZero(generator(), value);
        return;
    }
    define(value, toNumber(viewOfRange(value)));
}

void StringRangeReadEmitter::generateStringRangeParseInt(const ValueContext& value)
{
    if (value.representation() != Representation::Double) {
        failAndDefineZero(generator(), value);
        return;
    }
    ValueID radix = value->radixOperand();
    define(value, parseInt(viewOfRange(value), radix ? emitter<StringEmitter>().word32Of(radix) : word32(0)));
}

void StringRangeReadEmitter::generateStringRangeSubstringOrUndefined(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    CellPointer string = in<CellPointer>(value->stringOperand());
    CodeUnitRange range { strings.word32Of(value->startOperand()), strings.word32Of(value->endOperand()) };
    if (value.representation() == Representation::Cell) {
        define(value, strings.substring(value, string, range));
        return;
    }
    if (value.representation() != Representation::JSValue) {
        failAndDefineZero(generator(), value);
        return;
    }
    define(value, readIfInRange(m_out, notEqual(range.start, range.end), Representation::JSValue, [&] {
        return boxCell(strings.substring(value, string, range));
    }, [&] {
        return undefined();
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
