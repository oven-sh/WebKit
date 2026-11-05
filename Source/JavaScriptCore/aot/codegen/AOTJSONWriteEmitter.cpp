/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONWriteEmitter.h"

#if ENABLE(AOT)

#include "AOTCallEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringBuilderCursor.h"
#include "AOTStringBuilderStorage.h"
#include "StackManager.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(StringBuilderEmitter::maxInlineDigits <= jsonIntegerBytes);
static_assert(StringBuilderEmitter::maxInlineDigits <= jsonNumberBytes);
static_assert(sizeof(uint32_t) + sizeof(uint8_t) <= jsonBooleanBytes);

void JSONWriteEmitter::generateStringBuilderReserve(const ValueContext& value)
{
    CellPointer builder = builderOf(value);
    uint32_t size = value->size();
    LBasicBlock slow = newBlock();
    Join<Pointer> join(m_out);
    join.add(builders().tryReserve(builder, word32(static_cast<int32_t>(size)), slow));

    m_out.appendTo(slow);
    join.add(callOperation<Pointer>(value, RuntimeOperation::StringBuilderReserve, { builder, integer(encodeReservationOfAtMost(size)) }, { .isOnRarePath = true }));
    define(value, join.finish());
}

void JSONWriteEmitter::generateStringBuilderCommit(const ValueContext& value)
{
    builders().commit(builderOf(value), cursorOf(value));
}

void JSONWriteEmitter::generateJSONAppendKey(const ValueContext& value)
{
    if (!canWriteJSONKeyAtCursor(types(), value->string()) || (value->opensObject() && value->followsMember())) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    define(value, builders().writeBytes(cursorOf(value), bytesOfJSONKey(types(), value->string(), value->opensObject(), value->followsMember()).span()));
}

void JSONWriteEmitter::generateJSONAppendToken(const ValueContext& value)
{
    JSONToken token = value->jsonToken();
    switch (token) {
    case JSONToken::CloseObjectOverComma: {
        Pointer cursor = cursorOf(value);
        Word32 previous(m_out.load8ZeroExt32(builders().bytesAt(cursor, -1)));
        Pointer brace(m_out.sub(cursor.value(), zeroExtended(equal(previous, word32(','))).value()));
        LValue closingBrace = m_out.constInt32('}');
        m_out.store32As8(closingBrace, builders().bytesAt(brace));
        define(value, offsetBy(brace, 1));
        return;
    }
    case JSONToken::OpenObject:
    case JSONToken::CloseObject:
    case JSONToken::OpenArray:
    case JSONToken::CloseArray:
    case JSONToken::Comma:
    case JSONToken::Colon:
    case JSONToken::Null:
    case JSONToken::EmptyObject:
    case JSONToken::EmptyArray:
        define(value, builders().writeBytes(cursorOf(value), bytesOfJSONToken(token)));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void JSONWriteEmitter::generateJSONAppendBoolean(const ValueContext& value)
{
    Pointer cursor = cursorOf(value);
    Condition isTrue = in<Condition>(value->valueOperand());
    Word32 firstFourBytes = select<Word32>(isTrue, 't' | 'r' << 8 | 'u' << 16 | 'e' << 24, 'f' | 'a' << 8 | 'l' << 16 | 's' << 24);
    m_out.store32(firstFourBytes.value(), builders().bytesAt(cursor));
    LValue lastByteOfFalse = m_out.constInt32('e');
    m_out.store32As8(lastByteOfFalse, builders().bytesAt(cursor, 4));
    Pointer endOfFalse = offsetBy(cursor, 5);
    define(value, Pointer(m_out.sub(endOfFalse.value(), zeroExtended(isTrue).value())));
}

void JSONWriteEmitter::appendNumber(const ValueContext& value, bool isInteger)
{
    Pointer cursor = cursorOf(value);
    if (isInteger) {
        Integer whole = integerOf(value->valueOperand());
        define(value, either<Pointer>(below(whole, integer(StringBuilderEmitter::firstIntegerWrittenByCall)), [&]() -> Pointer {
            return builders().writeDigits(cursor, low32(whole));
        }, [&]() -> Pointer {
            return callLeaf<Pointer>(RuntimeOperation::JSONAppendInteger, { cursor, whole });
        }));
        return;
    }

    Number appended = in<Number>(value->valueOperand());
    Integer whole = emitter<ConversionEmitter>().toInteger(appended);
    Condition isWhole = equal(toNumber(whole), appended);
    Condition isWholeWithFewDigits = logicalAnd(isWhole, below(whole, integer(StringBuilderEmitter::firstIntegerWrittenByCall)));
    define(value, selectByBranch<Pointer>(isWholeWithFewDigits, [&]() -> Pointer {
        return builders().writeDigits(cursor, low32(whole));
    }, [&]() -> Pointer {
        return callLeaf<Pointer>(RuntimeOperation::JSONAppendNumber, { cursor, appended });
    }));
}

void JSONWriteEmitter::generateJSONAppendString(const ValueContext& value)
{
    define(value, callOperation<Pointer>(value, RuntimeOperation::JSONAppendString, { builderOf(value), cursorOf(value), in<CellPointer>(value->valueOperand()) }));
}

void JSONWriteEmitter::generateJSONAppendValue(const ValueContext& value)
{
    auto positionAndLevels = [&]() -> Word32 {
        uint32_t position = static_cast<uint32_t>(value->jsonPosition());
        if (!value->levelsOperand())
            return word32(static_cast<int32_t>(encodeJSONPositionAndLevels(position, 0)));
        Integer levels = integerOf(value->levelsOperand());
        if (levels.value()->hasInt())
            return word32(static_cast<int32_t>(encodeJSONPositionAndLevels(position, static_cast<uint32_t>(levels.value()->asInt()))));
        return bitOr(shiftLeft(low32(levels), static_cast<int32_t>(bitsOfJSONPosition)), word32(static_cast<int32_t>(position)));
    };
    Operand arguments[] = { builderOf(value), cursorOf(value), in<Boxed>(value->valueOperand()), positionAndLevels() };
    StubCallResult cursorAndOutcome = emitter<RuntimeCallEmitter>().callOperationReturningPair(value, RuntimeOperation::JSONAppendValue, arguments);
    defineResults(value, { Word32(m_out.castToInt32(cursorAndOutcome.second)), Pointer(cursorAndOutcome.first) });
}

void JSONWriteEmitter::generateJSONAppendObject(const ValueContext& value)
{
    CodeUnit serializer = conventions().serializerOf(value->layout());
    if (!serializer) {
        fail(value, InternalErrorKind::CallToFunctionWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    Operand arguments[] = { in<CellPointer>(value->objectOperand()), builderOf(value), low32(integerOf(value->levelsOperand())) };
    define(value, emitter<CallEmitter>().callDirect(value, serializer, arguments));
}

void JSONWriteEmitter::generateJSONHasStackSpace(const ValueContext& value)
{
    ptrdiff_t offsetOfMirror = value->failsAtWatchedLevels() ? AOT_INSTANCE_OFFSET(jsonLevels.limit) : AOT_INSTANCE_OFFSET(stackMirror);
    TypedPointer limit = instanceField(m_heaps.entire(AbstractHeap::RuntimeState), offsetOfMirror + StackManager::Mirror::offsetOfSoftStackLimit());
    Integer softStackLimit(m_out.load64(limit));
    Integer lowestAddressNeeded(m_out.sub(m_out.framePointer(), m_out.constInt64(stackReservedBelowJSONLevels)));
    defineTest(value, aboveOrEqual(lowestAddressNeeded, softStackLimit));
}

void JSONWriteEmitter::generateJSONEnterWatchedLevel(const ValueContext& value)
{
    Integer goesOn = callOperation<Integer>(value, RuntimeOperation::JSONEnterWatchedLevel, { builderOf(value), in<Boxed>(value->holderOperand()), Pointer(m_out.framePointer()) }, { .isOnRarePath = true });
    defineTest(value, isNotZero(goesOn));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
