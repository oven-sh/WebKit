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

uint16_t PrivateAccessEmitter::indexInLayoutRecord(const ValueContext& value)
{
    std::optional<uint16_t> index = m_placement.slot(value->layout(), value->field()).indexInLayoutRecord;
    if (!index)
        fail(value, InternalErrorKind::OpcodeWithoutCode);
    return index.value_or(0);
}

void PrivateAccessEmitter::generatePrivateGet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    define(value, Boxed(callStub(value, privateGet(), { object.value(), token.value() }, { .immediate = indexInLayoutRecord(value) }).first));
}

void PrivateAccessEmitter::storePrivateField(const ValueContext& value, RuntimeOperation operation)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    callVoidOperation(value, operation, { object, token, word32(indexInLayoutRecord(value)), stored });
}

void PrivateAccessEmitter::generatePrivateSet(const ValueContext& value)
{
    storePrivateField(value, RuntimeOperation::PrivateSet);
}

void PrivateAccessEmitter::generatePrivateDefine(const ValueContext& value)
{
    storePrivateField(value, RuntimeOperation::PrivateDefine);
}

void PrivateAccessEmitter::generatePrivateHas(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    CellPointer token = in<CellPointer>(value->tokenOperand());
    Boxed result = callOperation<Boxed>(value, RuntimeOperation::PrivateHas, { object, token, word32(indexInLayoutRecord(value)) });
    defineTest(value, equal(result, boxedConstant(jsBoolean(true))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
