/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPrivateAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTPlacement.h"
#include "AOTStubsPropertyKeys.h"
#include "JSCJSValueInlines.h"

namespace JSC { namespace AOT { namespace Typed {

PrivateAccessEmitter::PrivateAccessEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Word32 PrivateAccessEmitter::indexInLayoutRecord(const ValueContext& value)
{
    std::optional<uint16_t> index = m_placement.slot(value->layout(), value->field()).indexInLayoutRecord;
    if (!index)
        fail(value, InternalErrorKind::OpcodeWithoutCode);
    return word32(index.value_or(0));
}

void PrivateAccessEmitter::generatePrivateGet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    std::optional<uint16_t> index = m_placement.slot(value->layout(), value->field()).indexInLayoutRecord;
    if (!index)
        fail(value, InternalErrorKind::OpcodeWithoutCode);
    define(value, Boxed(callStub(value, privateGet(), { object.value(), token.value() }, { .immediate = index.value_or(0) }).first));
}

void PrivateAccessEmitter::generatePrivateSet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    callVoidOperation(value, RuntimeOperation::PrivateSet, { object, token, indexInLayoutRecord(value), stored });
}

void PrivateAccessEmitter::generatePrivateDefine(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    callVoidOperation(value, RuntimeOperation::PrivateDefine, { object, token, indexInLayoutRecord(value), stored });
}

void PrivateAccessEmitter::generatePrivateHas(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed result = callOperation<Boxed>(value, RuntimeOperation::PrivateHas, { object, token, indexInLayoutRecord(value) });
    defineTest(value, equal(result, boxedConstant(jsBoolean(true))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
