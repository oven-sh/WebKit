/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONWriteEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTRuntimeCallEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(StringBuilderEmitter::maxInlineDigits <= jsonIntegerBytes);
static_assert(sizeof(uint32_t) + sizeof(uint8_t) <= jsonBooleanBytes);

JSONWriteEmitter::JSONWriteEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void JSONWriteEmitter::generateStringBuilderReserve(const ValueContext& value)
{
    define(value, builders().reserve(value, builderOf(value), word32(value->size())));
}

void JSONWriteEmitter::generateStringBuilderCommit(const ValueContext& value)
{
    builders().commit(builderOf(value), cursorOf(value));
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
    case JSONToken::CloseObjectOverComma:
        define(value, closeObjectOverComma(cursorOf(value)));
        return;
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

void JSONWriteEmitter::generateJSONAppendInteger(const ValueContext& value)
{
    Pointer cursor = cursorOf(value);
    Integer appended = integerOf(value->valueOperand());
    LBasicBlock hasFewDigits = newBlock();
    LBasicBlock byCall = newBlock();
    Join<Pointer> join(m_out);

    branch(below(appended, integer(StringBuilderEmitter::firstIntegerWrittenByCall)), { normalEdge(hasFewDigits), unlikelyEdge(byCall) });

    m_out.appendTo(hasFewDigits);
    join.add(builders().writeDigits(cursor, low32(appended)));

    m_out.appendTo(byCall);
    join.add(callLeaf<Pointer>(RuntimeOperation::JSONAppendInteger, { cursor, appended }));

    define(value, join.finish());
}

void JSONWriteEmitter::generateJSONAppendNumber(const ValueContext& value)
{
    define(value, callLeaf<Pointer>(RuntimeOperation::JSONAppendNumber, { cursorOf(value), in<Number>(value->valueOperand()) }));
}

void JSONWriteEmitter::generateJSONAppendDate(const ValueContext& value)
{
    define(value, callLeaf<Pointer>(RuntimeOperation::JSONAppendDate, { cursorOf(value), in<Number>(value->valueOperand()) }));
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

Pointer JSONWriteEmitter::closeObjectOverComma(Pointer cursor)
{
    Word32 previous(m_out.load8ZeroExt32(builders().bytesAt(cursor, -1)));
    Pointer brace(m_out.sub(cursor.value(), zeroExtended(equal(previous, word32(','))).value()));
    m_out.store32As8(m_out.constInt32('}'), builders().bytesAt(brace));
    return offsetBy(brace, 1);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
