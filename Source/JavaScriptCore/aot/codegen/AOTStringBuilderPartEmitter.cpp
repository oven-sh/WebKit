/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderPartEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStringEmitter.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesTakenByStringBuilderParts = Primitive::All | Primitive::LiteralString | Primitive::LiteralSymbol;

static constexpr unsigned maxLengthOfTextWrittenByStores = 2 * sizeof(uint64_t);
static constexpr int32_t bytesReservedForBooleanPart = roundUpToMultipleOf<sizeof(uint64_t)>(sizeof("false") - 1);
static constexpr int32_t bytesReservedForNullishPart = roundUpToMultipleOf<sizeof(uint64_t)>(sizeof("undefined") - 1);
static_assert(maxLengthOfTextWrittenByStores <= StackStringBuilder::maxBytesWrittenPastLimit && StringBuilderEmitter::maxInlineDigits <= StackStringBuilder::maxBytesWrittenPastLimit);
static_assert(bytesReservedForBooleanPart <= static_cast<int32_t>(StackStringBuilder::maxBytesWrittenPastLimit) && bytesReservedForNullishPart <= static_cast<int32_t>(StackStringBuilder::maxBytesWrittenPastLimit));

static bool everyCellIsTextInStringBuilderParts(const Type& type)
{
    return (type.primitives & Primitive::AnyString) && !(type.primitives & (Primitive::BigInt | Primitive::AnySymbol));
}

static void emitArmsOfStringBuilderPart(Output& out, Condition condition, Machine::EdgeClass classOfSecond, NOESCAPE const Invocable<void()> auto& emitFirst, NOESCAPE const Invocable<void()> auto& emitSecond)
{
    LBasicBlock first = out.newBlock();
    LBasicBlock second = out.newBlock();
    LBasicBlock done = out.newBlock();
    ScopeFork fork = out.forkScope();
    out.branch(condition.value(), normalEdge(first), BlockEdge(second, classOfSecond));

    out.appendToArm(first, fork);
    emitFirst();
    out.jump(done);

    out.appendToArm(second, fork);
    emitSecond();
    out.jump(done);

    out.appendToJoin(done, fork);
}

static Pointer emitArmsOfStackStringBuilderPart(Output& out, Condition condition, Machine::EdgeClass classOfSecond, NOESCAPE const Invocable<Pointer()> auto& emitFirst, NOESCAPE const Invocable<Pointer()> auto& emitSecond)
{
    LBasicBlock first = out.newBlock();
    LBasicBlock second = out.newBlock();
    Join<Pointer> cursor(out);
    ScopeFork fork = out.forkScope();
    out.branch(condition.value(), normalEdge(first), BlockEdge(second, classOfSecond));

    out.appendToArm(first, fork);
    cursor.add(emitFirst());

    out.appendToArm(second, fork);
    cursor.add(emitSecond());
    return cursor.finishBehind(fork);
}

void StringBuilderPartEmitter::appendString(const OperationContext& context, CellPointer builder, CellPointer string)
{
    callStub(context, stringBuilderAppendString(), { builder.value(), string.value() });
}

void StringBuilderPartEmitter::appendPrimitive(const OperationContext& context, CellPointer builder, Boxed primitive, bool isOnRarePath)
{
    callVoidOperation(context, RuntimeOperation::StringBuilderAppend, { builder, word32(1), argumentBuffer(singleElementSpan(primitive)) }, { .isOnRarePath = isOnRarePath });
}

void StringBuilderPartEmitter::appendInWordsOrByCall(const OperationContext& context, CellPointer builder, int32_t bytesWritten, NOESCAPE const Invocable<Pointer(Pointer cursor)> auto& write, NOESCAPE const Invocable<Boxed()> auto& box)
{
    LBasicBlock byCall = newBlock();
    LBasicBlock done = newBlock();
    Pointer cursor = builders().tryReserve(builder, word32(bytesWritten), byCall);
    builders().commit(builder, write(cursor));
    m_out.jump(done);

    m_out.appendTo(byCall);
    appendPrimitive(context, builder, box(), true);
    m_out.jump(done);

    m_out.appendTo(done);
}

void StringBuilderPartEmitter::appendDigits(const OperationContext& context, CellPointer builder, Word32 value)
{
    appendInWordsOrByCall(context, builder, StringBuilderEmitter::maxInlineDigits, [&](Pointer cursor) {
        return builders().writeDigits(cursor, value);
    }, [&] {
        return emitter<ConversionEmitter>().boxInteger(zeroExtended(value), Range { 0, StringBuilderEmitter::firstIntegerWrittenByCall - 1 });
    });
}

void StringBuilderPartEmitter::appendNullish(const OperationContext& context, CellPointer builder, Condition isUndefined)
{
    appendInWordsOrByCall(context, builder, bytesReservedForNullishPart, [&](Pointer cursor) {
        return selectByBranch<Pointer>(isUndefined, [&] {
            return builders().writeBytes(cursor, "undefined"_s.span8());
        }, [&] {
            return builders().writeBytes(cursor, "null"_s.span8());
        });
    }, [&] {
        Boxed whenUndefined = undefined();
        return select(isUndefined, whenUndefined, boxedConstant(jsNull()));
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendConstant(const ValueContext& value)
{
    if (m_numberOfConstantsWrittenAhead) {
        --m_numberOfConstantsWrittenAhead;
        return;
    }

    StringEmitter& strings = emitter<StringEmitter>();
    CellPointer builder = in<CellPointer>(value->builderOperand());
    String text = strings.textOf(value->string());
    if (!text.is8Bit() || text.length() > maxLengthOfTextWrittenByStores) {
        appendString(value, builder, emitter<ConstantEmitter>().stringConstant(value, value->string()));
        return;
    }

    Vector<Latin1Character, maxLengthOfTextWrittenByStores> characters;
    characters.append(text.span8());
    strings.recordInlined(value->string());
    for (ValueID behind : value.operationsBehind()) {
        const Operation& next = operationAt(behind);
        if (next.opcode != Opcode::StringBuilderAppendConstant || next.builderOperand() != value->builderOperand())
            break;
        String nextText = strings.textOf(next.string());
        if (!nextText.is8Bit() || characters.size() + nextText.length() > maxLengthOfTextWrittenByStores)
            break;
        characters.append(nextText.span8());
        strings.recordInlined(next.string());
        ++m_numberOfConstantsWrittenAhead;
    }

    Pointer cursor = builders().reserve(value, builder, static_cast<uint32_t>(characters.size()));
    builders().commit(builder, builders().writeBytes(cursor, characters.span()));
}

void StringBuilderPartEmitter::holdIntegerToldToHaveFewDigits(const ValueContext& value, Integer appended)
{
    if (ElementAccessEmitter& elements = emitter<ElementAccessEmitter>(); elements.checksBounds())
        elements.stopUnlessInside(value, below(appended, StringBuilderEmitter::firstIntegerWrittenByCall));
}

void StringBuilderPartEmitter::generateStringBuilderAppendInteger(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Integer appended = integerOf(value->valueOperand());
    if (isProvenToHaveFewDigits(value->valueOperand())) {
        holdIntegerToldToHaveFewDigits(value, appended);
        appendDigits(value, builder, low32(appended));
        return;
    }
    emitArmsOfStringBuilderPart(m_out, below(appended, StringBuilderEmitter::firstIntegerWrittenByCall), Machine::EdgeClass::Unlikely, [&] {
        appendDigits(value, builder, low32(appended));
    }, [&] {
        appendPrimitive(value, builder, emitter<ConversionEmitter>().boxInteger(appended, typeOf(value->valueOperand()).integers));
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendNumber(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    if (representationOf(value->valueOperand()) == Representation::JSValue) {
        Boxed boxed = in<Boxed>(value->valueOperand());
        Integer untagged(m_out.sub(boxed.value(), m_numberTag));
        emitArmsOfStringBuilderPart(m_out, below(untagged, StringBuilderEmitter::firstIntegerWrittenByCall), Machine::EdgeClass::Normal, [&] {
            appendDigits(value, builder, low32(untagged));
        }, [&] {
            appendPrimitive(value, builder, boxed);
        });
        return;
    }
    Number appended = in<Number>(value->valueOperand());
    Integer truncated = emitter<ConversionEmitter>().truncateUnchecked(appended);
    Condition isWhole = equal(toNumber(truncated), appended);
    Condition hasFewDigits = logicalAnd(isWhole, below(truncated, StringBuilderEmitter::firstIntegerWrittenByCall));
    emitArmsOfStringBuilderPart(m_out, hasFewDigits, Machine::EdgeClass::Normal, [&] {
        appendDigits(value, builder, low32(truncated));
    }, [&] {
        callVoidOperation(value, RuntimeOperation::AppendNumberWithRadix, { builder, appended, word32(10) });
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendString(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    appendString(value, builder, in<CellPointer>(value->valueOperand()));
}

void StringBuilderPartEmitter::generateStringBuilderAppendStringRange(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    CellPointer builder = in<CellPointer>(value->builderOperand());
    CellPointer string = in<CellPointer>(value->stringOperand());
    Word32 start = strings.word32Of(value->startOperand());
    Word32 end = strings.word32Of(value->endOperand());
    Word32 length = sub(end, start);
    CharacterView view = strings.viewOf(value, value->stringOperand());
    strings.holdRangeInside(value, start, end);
    LBasicBlock byCall = newBlock();
    LBasicBlock done = newBlock();

    branchUnless(view.is8Bit, byCall);
    branchUnless(below(length, word32(StringBuilderStorage::minLengthAppendedByReference)), byCall);
    Pointer cursor = builders().tryReserve(builder, length, byCall);
    Pointer source = offsetBy(view.characters, zeroExtended(start), 0);
    builders().commit(builder, builders().copyLatin1Characters(cursor, source, length));
    ensureStillAliveHere(view.owner);
    m_out.jump(done);

    m_out.appendTo(byCall);
    callVoidOperation(value, RuntimeOperation::StringBuilderAppendStringRange, { builder, string, start, end }, { .isOnRarePath = true });
    m_out.jump(done);

    m_out.appendTo(done);
}

void StringBuilderPartEmitter::generateStringBuilderAppendBoolean(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Condition isTrue = in<Condition>(value->valueOperand());
    appendInWordsOrByCall(value, builder, bytesReservedForBooleanPart, [&](Pointer cursor) {
        Word32 firstFourBytes = select<Word32>(isTrue, 't' | 'r' << 8 | 'u' << 16 | 'e' << 24, 'f' | 'a' << 8 | 'l' << 16 | 's' << 24);
        m_out.store32(firstFourBytes.value(), builders().bytesAt(cursor));
        LValue lastByteOfFalse = m_out.constInt32('e');
        m_out.store32As8(lastByteOfFalse, builders().bytesAt(cursor, 4));
        Pointer endOfFalse = offsetBy(cursor, 5);
        return Pointer(m_out.sub(endOfFalse.value(), zeroExtended(isTrue).value()));
    }, [&] {
        return boxBoolean(isTrue);
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendCodeUnit(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Word32 codeUnit = emitter<StringEmitter>().word32Of(value->valueOperand());
    LBasicBlock byCall = newBlock();
    LBasicBlock done = newBlock();

    branchUnless(belowOrEqual(codeUnit, word32(maxSingleCharacterString)), byCall);
    Pointer cursor = builders().tryReserve(builder, word32(1), byCall);
    m_out.store32As8(codeUnit.value(), builders().bytesAt(cursor));
    builders().commit(builder, offsetBy(cursor, 1));
    m_out.jump(done);

    m_out.appendTo(byCall);
    appendString(value, builder, emitter<StringEmitter>().stringFromCodeUnit(value, codeUnit));
    m_out.jump(done);

    m_out.appendTo(done);
}

void StringBuilderPartEmitter::generateStringBuilderAppendStringOrNullish(const ValueContext& value)
{
    if (!typeOf(value->valueOperand()).hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull)) {
        fail(value, InternalErrorKind::OperandTypeMismatch);
        return;
    }
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Boxed appended = in<Boxed>(value->valueOperand());
    emitArmsOfStringBuilderPart(m_out, isCell(appended), Machine::EdgeClass::Unlikely, [&] {
        appendString(value, builder, unboxCell(appended));
    }, [&] {
        appendNullish(value, builder, equal(appended, undefined()));
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendValue(const ValueContext& value)
{
    ValueID appended = value->valueOperand();
    if (!typeOf(appended).hasOnlyPrimitives(primitivesTakenByStringBuilderParts)) {
        fail(value, InternalErrorKind::OperandTypeMismatch);
        return;
    }
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Boxed boxed = in<Boxed>(appended);
    auto appendTextOrPrimitive = [&] {
        if (!everyCellIsTextInStringBuilderParts(typeOf(appended))) {
            appendPrimitive(value, builder, boxed);
            return;
        }
        emitArmsOfStringBuilderPart(m_out, isCell(boxed), Machine::EdgeClass::Unlikely, [&] {
            appendString(value, builder, unboxCell(boxed));
        }, [&] {
            appendPrimitive(value, builder, boxed);
        });
    };
    if (typeOf(appended).primitives & Primitive::Number) {
        Integer untagged(m_out.sub(boxed.value(), m_numberTag));
        emitArmsOfStringBuilderPart(m_out, below(untagged, StringBuilderEmitter::firstIntegerWrittenByCall), Machine::EdgeClass::Normal, [&] {
            appendDigits(value, builder, low32(untagged));
        }, appendTextOrPrimitive);
        return;
    }
    appendTextOrPrimitive();
}

Pointer StringBuilderPartEmitter::appendString(const OperationContext& context, PlaceInFrame place, CellPointer string)
{
    return Pointer(callStub(context, stackStringBuilderAppendString(), { place.record.value(), place.cursor.value(), string.value() }).first);
}

Pointer StringBuilderPartEmitter::appendDigits(const OperationContext& context, PlaceInFrame place, Word32 value)
{
    return builders().advance(context, place.record, builders().writeDigits(place.cursor, value));
}

Pointer StringBuilderPartEmitter::appendPrimitive(const OperationContext& context, PlaceInFrame place, Boxed primitive, bool isOnRarePath)
{
    return callOperation<Pointer>(context, RuntimeOperation::StackStringBuilderAppend, { place.record, place.cursor, primitive }, { .isOnRarePath = isOnRarePath });
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendConstant(const ValueContext& value)
{
    if (m_numberOfConstantsWrittenAhead) {
        --m_numberOfConstantsWrittenAhead;
        define(value, in<Pointer>(value->cursorOperand()));
        return;
    }

    StringEmitter& strings = emitter<StringEmitter>();
    PlaceInFrame place = placeInFrameOf(value);
    String text = strings.textOf(value->string());
    if (!text.is8Bit() || text.length() > maxLengthOfTextWrittenByStores) {
        builders().defineCursorBehindPart(value, place.record, appendString(value, place, emitter<ConstantEmitter>().stringConstant(value, value->string())));
        return;
    }

    Vector<Latin1Character, maxLengthOfTextWrittenByStores> characters;
    characters.append(text.span8());
    strings.recordInlined(value->string());
    ValueID cursorInFront = value.id();
    for (ValueID behind : value.operationsBehind()) {
        const Operation& next = operationAt(behind);
        if (next.opcode != Opcode::StackStringBuilderAppendConstant || next.cursorOperand() != cursorInFront)
            break;
        String nextText = strings.textOf(next.string());
        if (!nextText.is8Bit() || characters.size() + nextText.length() > maxLengthOfTextWrittenByStores)
            break;
        characters.append(nextText.span8());
        strings.recordInlined(next.string());
        cursorInFront = behind;
        ++m_numberOfConstantsWrittenAhead;
    }
    builders().defineCursorBehindPart(value, place.record, builders().advance(value, place.record, builders().writeBytes(place.cursor, characters.span())), m_numberOfConstantsWrittenAhead);
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendInteger(const ValueContext& value)
{
    PlaceInFrame place = placeInFrameOf(value);
    Integer appended = integerOf(value->valueOperand());
    if (isProvenToHaveFewDigits(value->valueOperand())) {
        holdIntegerToldToHaveFewDigits(value, appended);
        builders().defineCursorBehindPart(value, place.record, appendDigits(value, place, low32(appended)));
        return;
    }
    builders().defineCursorBehindPart(value, place.record, emitArmsOfStackStringBuilderPart(m_out, below(appended, StringBuilderEmitter::firstIntegerWrittenByCall), Machine::EdgeClass::Unlikely, [&] {
        return appendDigits(value, place, low32(appended));
    }, [&] {
        return appendPrimitive(value, place, emitter<ConversionEmitter>().boxInteger(appended, typeOf(value->valueOperand()).integers));
    }));
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendNumber(const ValueContext& value)
{
    PlaceInFrame place = placeInFrameOf(value);
    if (representationOf(value->valueOperand()) == Representation::JSValue) {
        Boxed boxed = in<Boxed>(value->valueOperand());
        Integer untagged(m_out.sub(boxed.value(), m_numberTag));
        builders().defineCursorBehindPart(value, place.record, emitArmsOfStackStringBuilderPart(m_out, below(untagged, StringBuilderEmitter::firstIntegerWrittenByCall), Machine::EdgeClass::Normal, [&] {
            return appendDigits(value, place, low32(untagged));
        }, [&] {
            return appendPrimitive(value, place, boxed);
        }));
        return;
    }
    Number appended = in<Number>(value->valueOperand());
    Integer truncated = emitter<ConversionEmitter>().truncateUnchecked(appended);
    Condition isWhole = equal(toNumber(truncated), appended);
    Condition hasFewDigits = logicalAnd(isWhole, below(truncated, StringBuilderEmitter::firstIntegerWrittenByCall));
    builders().defineCursorBehindPart(value, place.record, emitArmsOfStackStringBuilderPart(m_out, hasFewDigits, Machine::EdgeClass::Normal, [&] {
        return appendDigits(value, place, low32(truncated));
    }, [&] {
        return appendPrimitive(value, place, emitter<ConversionEmitter>().boxDouble(appended));
    }));
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendString(const ValueContext& value)
{
    PlaceInFrame place = placeInFrameOf(value);
    auto append = [&](CellPointer string) {
        return appendString(value, place, string);
    };
    std::optional<Pointer> behind = builders().tryAppendFinishedBuilder(value, place.record, place.cursor, append);
    builders().defineCursorBehindPart(value, place.record, behind ? *behind : append(in<CellPointer>(value->valueOperand())));
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendStringRange(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    PlaceInFrame place = placeInFrameOf(value);
    CellPointer string = in<CellPointer>(value->stringOperand());
    Word32 start = strings.word32Of(value->startOperand());
    Word32 end = strings.word32Of(value->endOperand());
    Word32 length = sub(end, start);
    CharacterView view = strings.viewOf(value, value->stringOperand());
    strings.holdRangeInside(value, start, end);
    LBasicBlock byCall = newBlock();
    Join<Pointer> cursor(m_out);

    branchUnless(view.is8Bit, byCall);
    branchUnless(below(length, word32(StackStringBuilder::minLengthAppendedByReference)), byCall);
    branchUnless(belowOrEqual(offsetBy(place.cursor, zeroExtended(length), 0), builders().limitOf(place.record)), byCall);
    Pointer behind = builders().copyLatin1Characters(place.cursor, offsetBy(view.characters, zeroExtended(start), 0), length);
    ensureStillAliveHere(view.owner);
    cursor.add(behind);

    m_out.appendTo(byCall);
    cursor.add(callOperation<Pointer>(value, RuntimeOperation::StackStringBuilderAppendStringRange, { place.record, place.cursor, string, start, end }, { .isOnRarePath = true }));
    builders().defineCursorBehindPart(value, place.record, cursor.finish());
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendBoolean(const ValueContext& value)
{
    PlaceInFrame place = placeInFrameOf(value);
    Condition isTrue = in<Condition>(value->valueOperand());
    Word32 firstFourBytes = select<Word32>(isTrue, 't' | 'r' << 8 | 'u' << 16 | 'e' << 24, 'f' | 'a' << 8 | 'l' << 16 | 's' << 24);
    m_out.store32(firstFourBytes.value(), builders().bytesAt(place.cursor));
    LValue lastByteOfFalse = m_out.constInt32('e');
    m_out.store32As8(lastByteOfFalse, builders().bytesAt(place.cursor, 4));
    Pointer endOfFalse = offsetBy(place.cursor, 5);
    builders().defineCursorBehindPart(value, place.record, builders().advance(value, place.record, Pointer(m_out.sub(endOfFalse.value(), zeroExtended(isTrue).value()))));
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendCodeUnit(const ValueContext& value)
{
    PlaceInFrame place = placeInFrameOf(value);
    Word32 codeUnit = emitter<StringEmitter>().word32Of(value->valueOperand());
    builders().defineCursorBehindPart(value, place.record, emitArmsOfStackStringBuilderPart(m_out, belowOrEqual(codeUnit, word32(maxSingleCharacterString)), Machine::EdgeClass::Unlikely, [&] {
        m_out.store32As8(codeUnit.value(), builders().bytesAt(place.cursor));
        return builders().advance(value, place.record, offsetBy(place.cursor, 1));
    }, [&] {
        return callOperation<Pointer>(value, RuntimeOperation::StackStringBuilderAppendCodeUnit, { place.record, place.cursor, codeUnit }, { .isOnRarePath = true });
    }));
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendStringOrNullish(const ValueContext& value)
{
    if (!typeOf(value->valueOperand()).hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull)) {
        fail(value, InternalErrorKind::OperandTypeMismatch);
        define(value, in<Pointer>(value->cursorOperand()));
        return;
    }
    PlaceInFrame place = placeInFrameOf(value);
    Boxed appended = in<Boxed>(value->valueOperand());
    builders().defineCursorBehindPart(value, place.record, emitArmsOfStackStringBuilderPart(m_out, isCell(appended), Machine::EdgeClass::Unlikely, [&] {
        return appendString(value, place, unboxCell(appended));
    }, [&] {
        Pointer behind = selectByBranch<Pointer>(equal(appended, undefined()), [&] {
            return builders().writeBytes(place.cursor, "undefined"_s.span8());
        }, [&] {
            return builders().writeBytes(place.cursor, "null"_s.span8());
        });
        return builders().advance(value, place.record, behind);
    }));
}

void StringBuilderPartEmitter::generateStackStringBuilderAppendValue(const ValueContext& value)
{
    if (!typeOf(value->valueOperand()).hasOnlyPrimitives(primitivesTakenByStringBuilderParts)) {
        fail(value, InternalErrorKind::OperandTypeMismatch);
        define(value, in<Pointer>(value->cursorOperand()));
        return;
    }
    PlaceInFrame place = placeInFrameOf(value);
    Boxed boxed = in<Boxed>(value->valueOperand());
    auto appendTextOrPrimitive = [&] {
        if (!everyCellIsTextInStringBuilderParts(typeOf(value->valueOperand())))
            return appendPrimitive(value, place, boxed);
        return emitArmsOfStackStringBuilderPart(m_out, isCell(boxed), Machine::EdgeClass::Unlikely, [&] {
            return appendString(value, place, unboxCell(boxed));
        }, [&] {
            return appendPrimitive(value, place, boxed);
        });
    };
    if (typeOf(value->valueOperand()).primitives & Primitive::Number) {
        Integer untagged(m_out.sub(boxed.value(), m_numberTag));
        builders().defineCursorBehindPart(value, place.record, emitArmsOfStackStringBuilderPart(m_out, below(untagged, StringBuilderEmitter::firstIntegerWrittenByCall), Machine::EdgeClass::Normal, [&] {
            return appendDigits(value, place, low32(untagged));
        }, appendTextOrPrimitive));
        return;
    }
    builders().defineCursorBehindPart(value, place.record, appendTextOrPrimitive());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
