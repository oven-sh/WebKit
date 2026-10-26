/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONWriteEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTJSONParseEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringBuilderStorage.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(StringBuilderEmitter::maxInlineDigits <= jsonIntegerBytes);
static_assert(StringBuilderEmitter::maxInlineDigits <= jsonNumberBytes);
static_assert(sizeof(uint32_t) + sizeof(uint8_t) <= jsonBooleanBytes);

TypedPointer JSONWriteEmitter::listedHoldersOf(const ValueContext& value)
{
    return builders().storageField(builderOf(value), StringBuilderStorage::offsetOfJSONHolders());
}

void JSONWriteEmitter::generateJSONResetHolders(const ValueContext& value)
{
    m_out.storePtr(m_out.constIntPtr(0), listedHoldersOf(value));
}

void JSONWriteEmitter::generateJSONEnterHolder(const ValueContext& value)
{
    TypedPointer listed = listedHoldersOf(value);
    Pointer record(m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), m_proc.addStackSlot(sizeof(JSONHolderRecord))));
    m_out.storePtr(m_out.loadPtr(listed), builders().bytesAt(record, OBJECT_OFFSETOF(JSONHolderRecord, outer)));
    m_out.store64(in<Boxed>(value->holderOperand()).value(), builders().bytesAt(record, OBJECT_OFFSETOF(JSONHolderRecord, holder)));
    m_out.storePtr(record.value(), listed);
    define(value, record);
}

void JSONWriteEmitter::generateJSONLeaveHolder(const ValueContext& value)
{
    m_out.storePtr(m_out.loadPtr(builders().bytesAt(holdersOf(value), OBJECT_OFFSETOF(JSONHolderRecord, outer))), listedHoldersOf(value));
}

void JSONWriteEmitter::generateJSONListedHolders(const ValueContext& value)
{
    define(value, Pointer(m_out.loadPtr(listedHoldersOf(value))));
}

void JSONWriteEmitter::generateJSONRestoreHolders(const ValueContext& value)
{
    m_out.storePtr(holdersOf(value).value(), listedHoldersOf(value));
}

void JSONWriteEmitter::generateJSONEnterWatchedLevel(const ValueContext& value)
{
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::JSONEnterWatchedLevel, { builderOf(value), in<Boxed>(value->holderOperand()), emitter<JSONParseEmitter>().framePointerAtThisPoint() }, { .isOnRarePath = true }));
}

void JSONWriteEmitter::generateJSONAppendKey(const ValueContext& value)
{
    if (!canWriteJSONKeyAtCursor(m_program, value->string()) || (value->opensObject() && value->followsMember())) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    define(value, builders().writeBytes(cursorOf(value), bytesOfJSONKey(m_program, value.value()).span()));
}

void JSONWriteEmitter::generateJSONAppendToken(const ValueContext& value)
{
    JSONToken token = value->jsonToken();
    switch (token) {
    case JSONToken::CloseObjectOverComma: {
        Pointer cursor = cursorOf(value);
        Word32 previous(m_out.load8ZeroExt32(builders().bytesAt(cursor, -1)));
        Pointer brace(m_out.sub(cursor.value(), zeroExtended(equal(previous, word32(','))).value()));
        m_out.store32As8(m_out.constInt32('}'), builders().bytesAt(brace));
        define(value, offsetBy(brace, 1));
        return;
    }
    case JSONToken::OpenObject:
    case JSONToken::CloseObject:
    case JSONToken::OpenArray:
    case JSONToken::CloseArray:
    case JSONToken::Comma:
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
    Word32 firstFourBytes = select(isTrue, word32('t' | 'r' << 8 | 'u' << 16 | 'e' << 24), word32('f' | 'a' << 8 | 'l' << 16 | 's' << 24));
    m_out.store32(firstFourBytes.value(), builders().bytesAt(cursor));
    m_out.store32As8(m_out.constInt32('e'), builders().bytesAt(cursor, 4));
    define(value, Pointer(m_out.sub(offsetBy(cursor, 5).value(), zeroExtended(isTrue).value())));
}

void JSONWriteEmitter::appendNumber(const ValueContext& value, bool isInteger)
{
    Pointer cursor = cursorOf(value);
    Operand appended = isInteger ? Operand(integerOf(value->valueOperand())) : Operand(in<Number>(value->valueOperand()));
    Integer whole = isInteger ? appended.as<Integer>() : emitter<ConversionEmitter>().toInteger(appended.as<Number>());
    LBasicBlock hasFewDigits = newBlock();
    LBasicBlock byCall = newBlock();
    Join<Pointer> join(m_out);

    if (isInteger)
        branch(below(whole, integer(StringBuilderEmitter::firstIntegerWrittenByCall)), { normalEdge(hasFewDigits), unlikelyEdge(byCall) });
    else {
        Condition isWholeWithFewDigits = logicalAnd(equal(toNumber(whole), appended.as<Number>()), below(whole, integer(StringBuilderEmitter::firstIntegerWrittenByCall)));
        branch(isWholeWithFewDigits, { normalEdge(hasFewDigits), normalEdge(byCall) });
    }

    m_out.appendTo(hasFewDigits);
    join.add(builders().writeDigits(cursor, low32(whole)));

    m_out.appendTo(byCall);
    join.add(callLeaf<Pointer>(isInteger ? RuntimeOperation::JSONAppendInteger : RuntimeOperation::JSONAppendNumber, { cursor, appended }));

    define(value, join.finish());
}

void JSONWriteEmitter::generateJSONAppendString(const ValueContext& value)
{
    define(value, callOperation<Pointer>(value, RuntimeOperation::JSONAppendString, { builderOf(value), cursorOf(value), in<CellPointer>(value->valueOperand()) }));
}

void JSONWriteEmitter::generateJSONAppendValue(const ValueContext& value)
{
    Operand arguments[] = { builderOf(value), cursorOf(value), in<Boxed>(value->valueOperand()), word32(static_cast<int32_t>(value->jsonPosition())) };
    StubCallResult cursorAndOutcome = emitter<RuntimeCallEmitter>().callOperationReturningPair(value, RuntimeOperation::JSONAppendValue, arguments);
    defineResults(value, { Word32(m_out.castToInt32(cursorAndOutcome.second)), Pointer(cursorAndOutcome.first) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
