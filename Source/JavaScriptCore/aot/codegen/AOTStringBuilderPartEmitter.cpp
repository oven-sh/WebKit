/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderPartEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr int32_t bytesReservedForBoolean = roundUpToMultipleOf<8>(sizeof("false") - 1);
static constexpr int32_t bytesReservedForNullish = roundUpToMultipleOf<8>(sizeof("undefined") - 1);

static void emitDiamond(Output& out, Condition condition, B3::FrequencyClass frequencyOfSecond, NOESCAPE const Invocable<void()> auto& emitFirst, NOESCAPE const Invocable<void()> auto& emitSecond)
{
    LBasicBlock first = out.newBlock();
    LBasicBlock second = out.newBlock();
    LBasicBlock done = out.newBlock();
    out.branch(condition.value(), normalEdge(first), B3::FrequentedBlock(second, frequencyOfSecond));

    out.appendTo(first);
    emitFirst();
    out.jump(done);

    out.appendTo(second);
    emitSecond();
    out.jump(done);

    out.appendTo(done);
}

StringBuilderPartEmitter::StringBuilderPartEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void StringBuilderPartEmitter::appendView(CellPointer builder, const CharacterView& view, LBasicBlock byCall)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    branchUnless(view.is8Bit, byCall);
    branchUnless(below(view.length, word32(static_cast<int32_t>(build().limit(LimitID::MinLengthAppendedByReference)))), byCall);
    Pointer cursor = builders.tryReserve(builder, view.length, byCall);
    builders.commit(builder, builders.copyLatin1Characters(cursor, view.characters, view.length));
    ensureStillAliveHere(view.owner);
}

void StringBuilderPartEmitter::appendViewOrCall(CellPointer builder, const CharacterView& view, LBasicBlock byCall, NOESCAPE const Invocable<void()> auto& appendByCall)
{
    LBasicBlock done = newBlock();
    appendView(builder, view, byCall);
    m_out.jump(done);

    m_out.appendTo(byCall);
    appendByCall();
    m_out.jump(done);

    m_out.appendTo(done);
}

void StringBuilderPartEmitter::appendViewOfString(const OperationContext& context, CellPointer builder, CellPointer string, const CharacterView& view, LBasicBlock byCall)
{
    appendViewOrCall(builder, view, byCall, [&] {
        callVoidOperation(context, RuntimeOperation::StringBuilderAppendString, { builder, string }, { .isOnRarePath = true });
    });
}

void StringBuilderPartEmitter::appendString(const OperationContext& context, CellPointer builder, CellPointer string)
{
    LBasicBlock byCall = newBlock();
    branchUnless(logicalNot(isRopeString(string)), byCall);
    appendViewOfString(context, builder, string, emitter<StringEmitter>().viewOfFlatString(string), byCall);
}

void StringBuilderPartEmitter::appendConstant(const OperationContext& context, CellPointer builder, StringID id)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    String text = emitter<StringEmitter>().textOf(id);
    if (!text.is8Bit()) {
        fail(context, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    Pointer cursor = builders.reserve(context, builder, word32(static_cast<int32_t>(roundUpToMultipleOf<8>(text.length()))));
    builders.commit(builder, builders.writeBytes(cursor, text.span8()));
}

void StringBuilderPartEmitter::appendDigits(const OperationContext& context, CellPointer builder, Word32 value)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    Pointer cursor = builders.reserve(context, builder, word32(StringBuilderEmitter::maxInlineDigits));
    builders.commit(builder, builders.writeDigits(cursor, value));
}

void StringBuilderPartEmitter::appendNullish(const OperationContext& context, CellPointer builder, Condition isUndefined)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    LBasicBlock writeUndefined = newBlock();
    LBasicBlock writeNull = newBlock();
    Join<Pointer> end(m_out);
    Pointer cursor = builders.reserve(context, builder, word32(bytesReservedForNullish));
    m_out.branch(isUndefined.value(), normalEdge(writeUndefined), normalEdge(writeNull));

    m_out.appendTo(writeUndefined);
    end.add(builders.writeBytes(cursor, "undefined"_s.span8()));

    m_out.appendTo(writeNull);
    end.add(builders.writeBytes(cursor, "null"_s.span8()));

    builders.commit(builder, end.finish());
}

void StringBuilderPartEmitter::appendPrimitive(const OperationContext& context, CellPointer builder, Boxed primitive)
{
    callVoidOperation(context, RuntimeOperation::StringBuilderAppend, { builder, word32(1), argumentBuffer(singleElementSpan(primitive)) });
}

void StringBuilderPartEmitter::generateStringBuilderAppendConstant(const ValueContext& value)
{
    appendConstant(value, in<CellPointer>(value->builderOperand()), value->string());
}

void StringBuilderPartEmitter::generateStringBuilderAppendInteger(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Integer appended = integerOf(value->valueOperand());
    emitDiamond(m_out, below(appended, integer(StringBuilderEmitter::firstIntegerWrittenByCall)), B3::FrequencyClass::Unlikely, [&] {
        appendDigits(value, builder, low32(appended));
    }, [&] {
        appendPrimitive(value, builder, emitter<ConversionEmitter>().boxInteger(appended, typeOf(value->valueOperand()).integers));
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendNumber(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Number appended = in<Number>(value->valueOperand());
    Integer truncated = emitter<ConversionEmitter>().toInteger(appended);
    Condition hasFewDigits = logicalAnd(equal(toNumber(truncated), appended), below(truncated, integer(StringBuilderEmitter::firstIntegerWrittenByCall)));
    emitDiamond(m_out, hasFewDigits, B3::FrequencyClass::Normal, [&] {
        appendDigits(value, builder, low32(truncated));
    }, [&] {
        callVoidOperation(value, RuntimeOperation::AppendNumberWithRadix, { builder, appended, word32(10) });
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendBoolean(const ValueContext& value)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Condition isTrue = in<Condition>(value->valueOperand());
    Pointer cursor = builders.reserve(value, builder, word32(bytesReservedForBoolean));
    Word32 firstFourBytes = select(isTrue, word32('t' | 'r' << 8 | 'u' << 16 | 'e' << 24), word32('f' | 'a' << 8 | 'l' << 16 | 's' << 24));
    m_out.store32(firstFourBytes.value(), builders.bytesAt(cursor));
    m_out.store32As8(m_out.constInt32('e'), builders.bytesAt(cursor, 4));
    builders.commit(builder, Pointer(m_out.sub(offsetBy(cursor, 5).value(), zeroExtended(isTrue).value())));
}

void StringBuilderPartEmitter::generateStringBuilderAppendString(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    ValueID appended = value->valueOperand();
    if (!typeOf(appended).isOpaque(OpaqueKind::FlatString)) {
        appendString(value, builder, in<CellPointer>(appended));
        return;
    }
    appendViewOfString(value, builder, in<CellPointer>(appended), emitter<StringEmitter>().viewOf(value, appended), newBlock());
}

void StringBuilderPartEmitter::generateStringBuilderAppendStringRange(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    CellPointer builder = in<CellPointer>(value->builderOperand());
    CellPointer string = in<CellPointer>(value->stringOperand());
    CodeUnitRange range { strings.word32Of(value->startOperand()), strings.word32Of(value->endOperand()) };
    CharacterView view = strings.subview(strings.viewOf(value, value->stringOperand()), range);
    appendViewOrCall(builder, view, newBlock(), [&] {
        callVoidOperation(value, RuntimeOperation::StringBuilderAppendStringRange, { builder, string, range.start, range.end }, { .isOnRarePath = true });
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendStringOrNullish(const ValueContext& value)
{
    CellPointer builder = in<CellPointer>(value->builderOperand());
    Boxed appended = in<Boxed>(value->valueOperand());
    emitDiamond(m_out, isCell(appended), B3::FrequencyClass::Unlikely, [&] {
        appendString(value, builder, unboxCell(appended));
    }, [&] {
        appendNullish(value, builder, equal(appended, undefined()));
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendValue(const ValueContext& value)
{
    appendPrimitive(value, in<CellPointer>(value->builderOperand()), in<Boxed>(value->valueOperand()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
