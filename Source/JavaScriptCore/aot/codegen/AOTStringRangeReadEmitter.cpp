/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringRangeReadEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTStubsStrings.h"
#include "PureNaN.h"

namespace JSC { namespace AOT { namespace Typed {

static bool requireRepresentation(CodeGenerator& generator, const ValueContext& value, Representation representation)
{
    if (value.representation() == representation)
        return true;
    generator.fail(value, InternalErrorKind::OpcodeWithoutCode);
    generator.define(value, generator.emitter<ConstantEmitter>().zeroOf(value.representation()));
    return false;
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

CharacterView StringRangeReadEmitter::viewOfRange(const ValueContext& value)
{
    CharacterView view = strings().viewOf(value, value->stringOperand());
    ValueID start = value->startOperand();
    ValueID end = value->endOperand();
    if (!start && !end)
        return view;
    return strings().subview(view, { start ? strings().word32Of(start) : word32(0), end ? strings().word32Of(end) : view.length });
}

auto StringRangeReadEmitter::openIndexedRead(const ValueContext& value) -> IndexedRead
{
    CharacterView view = viewOfRange(value);
    // A negative position is above every length when it is read as unsigned.
    Operand position = get(value->positionOperand());
    if (position.is<Word32>())
        return { view, position.as<Word32>(), below(position.as<Word32>(), view.length) };
    Integer truncated = position.is<Number>() ? strings().truncateToIndex(position.as<Number>()) : integerOf(value->positionOperand());
    return { view, low32(truncated), below(truncated, zeroExtended(view.length)) };
}

Number StringRangeReadEmitter::toNumber(const CharacterView& view)
{
    Number result = strings().selectByWidth<Number>(view.is8Bit, [&](bool is8Bit) {
        return callLeaf<Number>(is8Bit ? RuntimeOperation::StringToNumber8 : RuntimeOperation::StringToNumber16, { view.characters, view.length });
    }, B3::FrequencyClass::Normal);
    ensureStillAliveHere(view.owner);
    return result;
}

Number StringRangeReadEmitter::parseInt(const CharacterView& view, Word32 radix)
{
    Number result = strings().selectByWidth<Number>(view.is8Bit, [&](bool is8Bit) {
        return callLeaf<Number>(is8Bit ? RuntimeOperation::ParseInt8 : RuntimeOperation::ParseInt16, { view.characters, view.length, radix });
    }, B3::FrequencyClass::Normal);
    ensureStillAliveHere(view.owner);
    return result;
}

void StringRangeReadEmitter::generateStringRangeCodeUnitOrNaN(const ValueContext& value)
{
    IndexedRead read = openIndexedRead(value);
    if (value.representation() == Representation::Int32) {
        define(value, strings().codeUnitAt(read.view, read.index));
        return;
    }
    if (!requireRepresentation(generator(), value, Representation::Double))
        return;
    define(value, readIfInRange(m_out, read.isInRange, Representation::Double, [&] {
        return PrimitiveEmitter::toNumber(strings().codeUnitAt(read.view, read.index));
    }, [&] {
        return number(PNaN);
    }));
}

void StringRangeReadEmitter::generateStringRangeCodeUnitOrMinusOne(const ValueContext& value)
{
    if (!requireRepresentation(generator(), value, Representation::Int32))
        return;
    IndexedRead read = openIndexedRead(value);
    define(value, readIfInRange(m_out, read.isInRange, Representation::Int32, [&] {
        return strings().codeUnitAt(read.view, read.index);
    }, [&] {
        return word32(-1);
    }));
}

// A negative position is above every length when it is read as unsigned.
Integer StringRangeReadEmitter::readCodePointByStub(const ValueContext& value)
{
    Operand position = get(value->positionOperand());
    Integer index = integer(0);
    if (position.is<Word32>())
        index = zeroExtended(position.as<Word32>());
    else
        index = position.is<Number>() ? strings().truncateToIndex(position.as<Number>()) : integerOf(value->positionOperand());
    return Integer(callStub(value, stringCodePointAt(), { in<CellPointer>(value->stringOperand()).value(), index.value() }).first);
}

void StringRangeReadEmitter::generateStringRangeCodePointOrUndefined(const ValueContext& value)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    uint64_t undefinedMarker = ConversionEmitter::markerBits(Primitive::Undefined, Representation::OptionalInt64);
    bool readsWholeString = !value->startOperand() && !value->endOperand();
    bool hasCodeForResult = value.representation() == Representation::Int32 || value.representation() == Representation::OptionalInt64;
    if (value.shape() != CodeShape::Fast && readsWholeString && hasCodeForResult) {
        RELEASE_ASSERT(undefinedMarker == static_cast<uint64_t>(undefinedInteger));
        Integer codePointOrMarker = readCodePointByStub(value);
        if (value.representation() == Representation::Int32)
            define(value, low32(codePointOrMarker));
        else
            define(value, conversions.fromBits(codePointOrMarker, Representation::OptionalInt64));
        return;
    }

    IndexedRead read = openIndexedRead(value);
    if (value.representation() == Representation::Int32) {
        define(value, strings().codePointAt(read.view, read.index));
        return;
    }
    if (!requireRepresentation(generator(), value, Representation::OptionalInt64))
        return;
    define(value, readIfInRange(m_out, read.isInRange, Representation::OptionalInt64, [&] {
        return conversions.fromBits(zeroExtended(strings().codePointAt(read.view, read.index)), Representation::OptionalInt64);
    }, [&] {
        return conversions.fromBits(integer(static_cast<int64_t>(undefinedMarker)), Representation::OptionalInt64);
    }));
}

CellPointer StringRangeReadEmitter::readCharacter(const ValueContext& value, const IndexedRead& read)
{
    Word32 codeUnit = strings().codeUnitAt(read.view, read.index);
    return strings().stringFromCodeUnit(value, codeUnit);
}

void StringRangeReadEmitter::generateStringRangeCharacterOrEmpty(const ValueContext& value)
{
    if (!requireRepresentation(generator(), value, Representation::Cell))
        return;
    IndexedRead read = openIndexedRead(value);
    define(value, readIfInRange(m_out, read.isInRange, Representation::Cell, [&] {
        return readCharacter(value, read);
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
    Boxed whenOutOfRange = undefined();
    if (value.representation() == Representation::Cell) {
        define(value, readCharacter(value, read));
        return;
    }
    if (!requireRepresentation(generator(), value, Representation::JSValue))
        return;
    define(value, readIfInRange(m_out, read.isInRange, Representation::JSValue, [&] {
        return boxCell(readCharacter(value, read));
    }, [&] {
        return whenOutOfRange;
    }));
}

// A range that ends a constant number of units behind its start.
static std::optional<int64_t> findLengthWrittenInRange(const Function& function, const Type& typeOfEnd, ValueID start, ValueID end)
{
    if (!start || !end || !typeOfEnd.isInt32())
        return std::nullopt;
    const Value& sum = function.at(end);
    if (sum.opcode != Opcode::Add || sum.operand(0) != start || function.at(sum.operand(1)).opcode != Opcode::Integer)
        return std::nullopt;
    return function.at(sum.operand(1)).integer();
}

void StringRangeReadEmitter::generateStringRangeEqualsLiteral(const ValueContext& value)
{
    CharacterView view = viewOfRange(value);
    unsigned length = strings().textOf(value->literal()).length();
    if (!length) {
        ensureStillAliveHere(view.owner);
        defineTest(value, isZero(view.length));
        return;
    }
    if (value->endOperand() && findLengthWrittenInRange(m_function, typeOf(value->endOperand()), value->startOperand(), value->endOperand()) == length) {
        defineTest(value, strings().hasTextAt(view, word32(0), value->literal()));
        return;
    }

    LBasicBlock hasSameLength = newBlock();
    LBasicBlock hasOtherLength = newBlock();
    Join<Condition> join(m_out);
    m_out.branch(equal(view.length, word32(length)).value(), normalEdge(hasSameLength), normalEdge(hasOtherLength));
    m_out.appendTo(hasSameLength);
    join.add(strings().hasTextAt(view, word32(0), value->literal()));
    m_out.appendTo(hasOtherLength);
    join.add(boolean(false));
    defineTest(value, join.finish());
}

void StringRangeReadEmitter::generateStringRangeToNumber(const ValueContext& value)
{
    if (!requireRepresentation(generator(), value, Representation::Double))
        return;
    define(value, toNumber(viewOfRange(value)));
}

void StringRangeReadEmitter::generateStringRangeParseInt(const ValueContext& value)
{
    if (!requireRepresentation(generator(), value, Representation::Double))
        return;
    ValueID radix = value->radixOperand();
    CharacterView view = viewOfRange(value);
    define(value, parseInt(view, radix ? strings().word32Of(radix) : word32(0)));
}

void StringRangeReadEmitter::generateStringRangeSubstringOrUndefined(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    CodeUnitRange range { strings().word32Of(value->startOperand()), strings().word32Of(value->endOperand()) };
    if (value.representation() == Representation::Cell) {
        define(value, strings().substring(value, string, range));
        return;
    }
    if (!requireRepresentation(generator(), value, Representation::JSValue))
        return;
    define(value, readIfInRange(m_out, notEqual(range.start, range.end), Representation::JSValue, [&] {
        return boxCell(strings().substring(value, string, range));
    }, [&] {
        return undefined();
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
