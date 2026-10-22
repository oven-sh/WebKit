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

StringBuilderPartEmitter::StringBuilderPartEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

auto StringBuilderPartEmitter::builderOf(const ValueContext& value) -> Builder
{
    if (const StringBuilderEmitter::StackBuilder* onStack = emitter<StringBuilderEmitter>().stackBuilderFor(value->builderOperand()))
        return { { }, onStack };
    return { in<CellPointer>(value->builderOperand()), nullptr };
}

void StringBuilderPartEmitter::appendView(Builder builder, const CharacterView& view, LBasicBlock byCall)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    branchUnless(view.is8Bit, byCall);
    branchUnless(below(view.length, word32(static_cast<int32_t>(build().limit(LimitID::MinLengthAppendedByReference)))), byCall);
    if (builder.onStack) {
        Pointer cursor = builders.cursorOf(*builder.onStack);
        branchUnless(belowOrEqual(offsetBy(cursor, zeroExtended(view.length), 0), builders.limitOf(*builder.onStack)), byCall);
        builders.setCursor(*builder.onStack, builders.copyLatin1Characters(cursor, view.characters, view.length));
    } else {
        Pointer cursor = builders.tryReserve(builder.storage, view.length, byCall);
        builders.commit(builder.storage, builders.copyLatin1Characters(cursor, view.characters, view.length));
    }
    ensureStillAliveHere(view.owner);
}

void StringBuilderPartEmitter::appendViewOrCall(Builder builder, const CharacterView& view, LBasicBlock byCall, NOESCAPE const Invocable<void()> auto& appendByCall)
{
    LBasicBlock done = newBlock();
    appendView(builder, view, byCall);
    m_out.jump(done);

    m_out.appendTo(byCall);
    appendByCall();
    m_out.jump(done);

    m_out.appendTo(done);
}

void StringBuilderPartEmitter::appendString(const OperationContext& context, Builder builder, CellPointer string)
{
    if (builder.onStack) {
        StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
        builders.setCursor(*builder.onStack, Pointer(callStub(context, stackStringBuilderAppendString(), { builders.addressOf(*builder.onStack).value(), builders.cursorOf(*builder.onStack).value(), string.value() }).first));
        return;
    }
    callStub(context, stringBuilderAppendString(), { builder.storage.value(), string.value() });
}

void StringBuilderPartEmitter::appendConstant(const OperationContext& context, Builder builder, StringID id)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    String text = emitter<StringEmitter>().textOf(id);
    if (!text.is8Bit() || (builder.onStack && roundUpToMultipleOf<sizeof(uint64_t)>(text.length()) > StackStringBuilder::maxBytesWrittenPastLimit)) {
        fail(context, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    if (builder.onStack) {
        builders.advance(context, *builder.onStack, builders.writeBytes(builders.cursorOf(*builder.onStack), text.span8()));
        return;
    }
    Pointer cursor = builders.reserve(context, builder.storage, text.length());
    builders.commit(builder.storage, builders.writeBytes(cursor, text.span8()));
}

// How many characters are appended is known only when they are written, and fewer than the bytes that are written. Where those bytes have no room, the operation appends just the characters:
// near the longest text it is the characters that count.
void StringBuilderPartEmitter::appendInWordsOrByCall(const OperationContext& context, Builder builder, int32_t bytesWritten, NOESCAPE const Invocable<Pointer(Pointer cursor)> auto& write, NOESCAPE const Invocable<Boxed()> auto& box)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    if (builder.onStack) {
        builders.advance(context, *builder.onStack, write(builders.cursorOf(*builder.onStack)));
        return;
    }
    LBasicBlock byCall = newBlock();
    LBasicBlock done = newBlock();
    Pointer cursor = builders.tryReserve(builder.storage, word32(bytesWritten), byCall);
    builders.commit(builder.storage, write(cursor));
    m_out.jump(done);

    m_out.appendTo(byCall);
    appendPrimitive(context, builder, box(), true);
    m_out.jump(done);

    m_out.appendTo(done);
}

void StringBuilderPartEmitter::appendDigits(const OperationContext& context, Builder builder, Word32 value)
{
    appendInWordsOrByCall(context, builder, StringBuilderEmitter::maxInlineDigits, [&](Pointer cursor) {
        return emitter<StringBuilderEmitter>().writeDigits(cursor, value);
    }, [&] {
        return emitter<ConversionEmitter>().boxInteger(zeroExtended(value), Range { 0, StringBuilderEmitter::firstIntegerWrittenByCall - 1 });
    });
}

void StringBuilderPartEmitter::appendNullish(const OperationContext& context, Builder builder, Condition isUndefined)
{
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    appendInWordsOrByCall(context, builder, bytesReservedForNullish, [&](Pointer cursor) {
        LBasicBlock writeUndefined = newBlock();
        LBasicBlock writeNull = newBlock();
        Join<Pointer> end(m_out);
        m_out.branch(isUndefined.value(), normalEdge(writeUndefined), normalEdge(writeNull));

        m_out.appendTo(writeUndefined);
        end.add(builders.writeBytes(cursor, "undefined"_s.span8()));

        m_out.appendTo(writeNull);
        end.add(builders.writeBytes(cursor, "null"_s.span8()));
        return end.finish();
    }, [&] {
        return Boxed(m_out.select(isUndefined.value(), undefined().value(), boxedConstant(jsNull()).value()));
    });
}

void StringBuilderPartEmitter::appendPrimitive(const OperationContext& context, Builder builder, Boxed primitive, bool isOnRarePath)
{
    if (builder.onStack) {
        StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
        builders.setCursor(*builder.onStack, callOperation<Pointer>(context, RuntimeOperation::StackStringBuilderAppend, { builders.addressOf(*builder.onStack), builders.cursorOf(*builder.onStack), primitive }, { .isOnRarePath = isOnRarePath }));
        return;
    }
    callVoidOperation(context, RuntimeOperation::StringBuilderAppend, { builder.storage, word32(1), argumentBuffer(singleElementSpan(primitive)) }, { .isOnRarePath = isOnRarePath });
}

void StringBuilderPartEmitter::generateStringBuilderAppendConstant(const ValueContext& value)
{
    appendConstant(value, builderOf(value), value->string());
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
    StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
    Builder builder = builderOf(value);
    Condition isTrue = in<Condition>(value->valueOperand());
    appendInWordsOrByCall(value, builder, bytesReservedForBoolean, [&](Pointer cursor) {
        Word32 firstFourBytes = select(isTrue, word32('t' | 'r' << 8 | 'u' << 16 | 'e' << 24), word32('f' | 'a' << 8 | 'l' << 16 | 's' << 24));
        m_out.store32(firstFourBytes.value(), builders.bytesAt(cursor));
        m_out.store32As8(m_out.constInt32('e'), builders.bytesAt(cursor, 4));
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
    appendViewOrCall(builder, view, newBlock(), [&] {
        if (builder.onStack) {
            StringBuilderEmitter& builders = emitter<StringBuilderEmitter>();
            builders.setCursor(*builder.onStack, callOperation<Pointer>(value, RuntimeOperation::StackStringBuilderAppendStringRange, { builders.addressOf(*builder.onStack), builders.cursorOf(*builder.onStack), string, range.start, range.end }, { .isOnRarePath = true }));
            return;
        }
        callVoidOperation(value, RuntimeOperation::StringBuilderAppendStringRange, { builder.storage, string, range.start, range.end }, { .isOnRarePath = true });
    });
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
