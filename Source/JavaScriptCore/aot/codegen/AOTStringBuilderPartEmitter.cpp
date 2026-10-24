/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderPartEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTStringBuilderStorage.h"
#include "AOTStubsStrings.h"
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr int32_t bytesReservedForBoolean = roundUpToMultipleOf<8>(sizeof("false") - 1);
static constexpr int32_t bytesReservedForNullish = roundUpToMultipleOf<8>(sizeof("undefined") - 1);
static_assert(bytesReservedForNullish <= StackStringBuilder::maxBytesWrittenPastLimit && StringBuilderEmitter::maxInlineDigits <= StackStringBuilder::maxBytesWrittenPastLimit);

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

auto StringBuilderPartEmitter::builderOf(const ValueContext& value) -> Builder
{
    if (const StringBuilderEmitter::StackBuilder* onStack = builders().stackBuilderFor(value->builderOperand()))
        return { { }, onStack };
    return { in<CellPointer>(value->builderOperand()), nullptr };
}

void StringBuilderPartEmitter::appendString(const OperationContext& context, Builder builder, CellPointer string)
{
    if (builder.onStack) {
        builders().setCursor(*builder.onStack, Pointer(callStub(context, stackStringBuilderAppendString(), { builders().addressOf(*builder.onStack).value(), builders().cursorOf(*builder.onStack).value(), string.value() }).first));
        return;
    }
    callStub(context, stringBuilderAppendString(), { builder.storage.value(), string.value() });
}

// How many characters are appended is known only when they are written, and fewer than the bytes that are written.
void StringBuilderPartEmitter::appendInWordsOrByCall(const OperationContext& context, Builder builder, int32_t bytesWritten, NOESCAPE const Invocable<Pointer(Pointer cursor)> auto& write, NOESCAPE const Invocable<Boxed()> auto& box)
{
    if (builder.onStack) {
        builders().advance(context, *builder.onStack, write(builders().cursorOf(*builder.onStack)));
        return;
    }
    LBasicBlock byCall = newBlock();
    LBasicBlock done = newBlock();
    Pointer cursor = builders().tryReserve(builder.storage, word32(bytesWritten), byCall);
    builders().commit(builder.storage, write(cursor));
    m_out.jump(done);

    m_out.appendTo(byCall);
    appendPrimitive(context, builder, box(), true);
    m_out.jump(done);

    m_out.appendTo(done);
}

void StringBuilderPartEmitter::appendDigits(const OperationContext& context, Builder builder, Word32 value)
{
    appendInWordsOrByCall(context, builder, StringBuilderEmitter::maxInlineDigits, [&](Pointer cursor) {
        return builders().writeDigits(cursor, value);
    }, [&] {
        return emitter<ConversionEmitter>().boxInteger(zeroExtended(value), Range { 0, StringBuilderEmitter::firstIntegerWrittenByCall - 1 });
    });
}

void StringBuilderPartEmitter::appendNullish(const OperationContext& context, Builder builder, Condition isUndefined)
{
    appendInWordsOrByCall(context, builder, bytesReservedForNullish, [&](Pointer cursor) {
        LBasicBlock writeUndefined = newBlock();
        LBasicBlock writeNull = newBlock();
        Join<Pointer> end(m_out);
        m_out.branch(isUndefined.value(), normalEdge(writeUndefined), normalEdge(writeNull));

        m_out.appendTo(writeUndefined);
        end.add(builders().writeBytes(cursor, "undefined"_s.span8()));

        m_out.appendTo(writeNull);
        end.add(builders().writeBytes(cursor, "null"_s.span8()));
        return end.finish();
    }, [&] {
        return Boxed(m_out.select(isUndefined.value(), undefined().value(), boxedConstant(jsNull()).value()));
    });
}

void StringBuilderPartEmitter::appendPrimitive(const OperationContext& context, Builder builder, Boxed primitive, bool isOnRarePath)
{
    if (builder.onStack) {
        builders().setCursor(*builder.onStack, callOperation<Pointer>(context, RuntimeOperation::StackStringBuilderAppend, { builders().addressOf(*builder.onStack), builders().cursorOf(*builder.onStack), primitive }, { .isOnRarePath = isOnRarePath }));
        return;
    }
    callVoidOperation(context, RuntimeOperation::StringBuilderAppend, { builder.storage, word32(1), argumentBuffer(singleElementSpan(primitive)) }, { .isOnRarePath = isOnRarePath });
}

void StringBuilderPartEmitter::generateStringBuilderAppendConstant(const ValueContext& value)
{
    Builder builder = builderOf(value);
    String text = emitter<StringEmitter>().textOf(value->string());
    if (!text.is8Bit() || (builder.onStack && roundUpToMultipleOf<sizeof(uint64_t)>(text.length()) > StackStringBuilder::maxBytesWrittenPastLimit)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    if (builder.onStack) {
        builders().advance(value, *builder.onStack, builders().writeBytes(builders().cursorOf(*builder.onStack), text.span8()));
        return;
    }
    Pointer cursor = builders().reserve(value, builder.storage, text.length());
    builders().commit(builder.storage, builders().writeBytes(cursor, text.span8()));
}

void StringBuilderPartEmitter::generateStringBuilderAppendInteger(const ValueContext& value)
{
    Builder builder = builderOf(value);
    Integer appended = integerOf(value->valueOperand());
    emitDiamond(m_out, below(appended, integer(StringBuilderEmitter::firstIntegerWrittenByCall)), B3::FrequencyClass::Unlikely, [&] {
        appendDigits(value, builder, low32(appended));
    }, [&] {
        appendPrimitive(value, builder, emitter<ConversionEmitter>().boxInteger(appended, typeOf(value->valueOperand()).integers));
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendNumber(const ValueContext& value)
{
    Builder builder = builderOf(value);
    if (representationOf(value->valueOperand()) == Representation::JSValue) {
        Boxed boxed = in<Boxed>(value->valueOperand());
        Integer untagged(m_out.sub(boxed.value(), m_numberTag));
        emitDiamond(m_out, below(untagged, integer(StringBuilderEmitter::firstIntegerWrittenByCall)), B3::FrequencyClass::Normal, [&] {
            appendDigits(value, builder, low32(untagged));
        }, [&] {
            appendPrimitive(value, builder, boxed);
        });
        return;
    }
    Number appended = in<Number>(value->valueOperand());
    Integer truncated = emitter<ConversionEmitter>().toInteger(appended);
    Condition hasFewDigits = logicalAnd(equal(toNumber(truncated), appended), below(truncated, integer(StringBuilderEmitter::firstIntegerWrittenByCall)));
    emitDiamond(m_out, hasFewDigits, B3::FrequencyClass::Normal, [&] {
        appendDigits(value, builder, low32(truncated));
    }, [&] {
        if (builder.onStack)
            appendPrimitive(value, builder, emitter<ConversionEmitter>().boxDouble(appended));
        else
            callVoidOperation(value, RuntimeOperation::AppendNumberWithRadix, { builder.storage, appended, word32(10) });
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendBoolean(const ValueContext& value)
{
    Builder builder = builderOf(value);
    Condition isTrue = in<Condition>(value->valueOperand());
    appendInWordsOrByCall(value, builder, bytesReservedForBoolean, [&](Pointer cursor) {
        Word32 firstFourBytes = select(isTrue, word32('t' | 'r' << 8 | 'u' << 16 | 'e' << 24), word32('f' | 'a' << 8 | 'l' << 16 | 's' << 24));
        m_out.store32(firstFourBytes.value(), builders().bytesAt(cursor));
        m_out.store32As8(m_out.constInt32('e'), builders().bytesAt(cursor, 4));
        return Pointer(m_out.sub(offsetBy(cursor, 5).value(), zeroExtended(isTrue).value()));
    }, [&] {
        return boxBoolean(isTrue);
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendString(const ValueContext& value)
{
    appendString(value, builderOf(value), in<CellPointer>(value->valueOperand()));
}

void StringBuilderPartEmitter::generateStringBuilderAppendStringRange(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    Builder builder = builderOf(value);
    CellPointer string = in<CellPointer>(value->stringOperand());
    CodeUnitRange range { strings.word32Of(value->startOperand()), strings.word32Of(value->endOperand()) };
    CharacterView view = strings.subview(strings.viewOf(value, value->stringOperand()), range);
    LBasicBlock byCall = newBlock();
    LBasicBlock done = newBlock();

    branchUnless(view.is8Bit, byCall);
    branchUnless(below(view.length, word32(static_cast<int32_t>(build().limit(LimitID::MinLengthAppendedByReference)))), byCall);
    if (builder.onStack) {
        Pointer cursor = builders().cursorOf(*builder.onStack);
        branchUnless(belowOrEqual(offsetBy(cursor, zeroExtended(view.length), 0), builders().limitOf(*builder.onStack)), byCall);
        builders().setCursor(*builder.onStack, builders().copyLatin1Characters(cursor, view.characters, view.length));
    } else {
        Pointer cursor = builders().tryReserve(builder.storage, view.length, byCall);
        builders().commit(builder.storage, builders().copyLatin1Characters(cursor, view.characters, view.length));
    }
    ensureStillAliveHere(view.owner);
    m_out.jump(done);

    m_out.appendTo(byCall);
    if (builder.onStack)
        builders().setCursor(*builder.onStack, callOperation<Pointer>(value, RuntimeOperation::StackStringBuilderAppendStringRange, { builders().addressOf(*builder.onStack), builders().cursorOf(*builder.onStack), string, range.start, range.end }, { .isOnRarePath = true }));
    else
        callVoidOperation(value, RuntimeOperation::StringBuilderAppendStringRange, { builder.storage, string, range.start, range.end }, { .isOnRarePath = true });
    m_out.jump(done);

    m_out.appendTo(done);
}

void StringBuilderPartEmitter::generateStringBuilderAppendStringOrNullish(const ValueContext& value)
{
    Builder builder = builderOf(value);
    Boxed appended = in<Boxed>(value->valueOperand());
    emitDiamond(m_out, isCell(appended), B3::FrequencyClass::Unlikely, [&] {
        appendString(value, builder, unboxCell(appended));
    }, [&] {
        appendNullish(value, builder, equal(appended, undefined()));
    });
}

void StringBuilderPartEmitter::generateStringBuilderAppendValue(const ValueContext& value)
{
    appendPrimitive(value, builderOf(value), in<Boxed>(value->valueOperand()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
