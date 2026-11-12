/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTErrorBuiltinEmitter.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

ErrorBuiltinEmitter::ErrorBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void ErrorBuiltinEmitter::generateNewError(const InlineCall& call)
{
    auto argument = [&](unsigned index) {
        ValueID id = call.argumentOrNull(index);
        return id ? boxedOperand(call.context(), id) : undefined();
    };
    ErrorKind kind = call.parameter<ErrorKind>(0);
    Boxed message = argument(0);
    Boxed options = argument(1);
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::NewError, { word32(static_cast<int32_t>(kind)), message, options }, { .capturesStackTrace = true }));
}

void ErrorBuiltinEmitter::generateErrorIsError(const InlineCall& call)
{
    ValueID argument = call.argumentOrNull(0);
    if (!argument || !typeOf(argument).hasObjects()) {
        finishInlineCall(call, boolean(false));
        return;
    }
    LBasicBlock isCellCase = newBlock();
    LBasicBlock isNotCell = newBlock();
    Join<Condition> join(m_out);
    Boxed value = in<Boxed>(argument);
    branch(isCell(value), { normalEdge(isCellCase), normalEdge(isNotCell) });

    m_out.appendTo(isCellCase);
    join.add(hasCellType(unboxCell(value), ErrorInstanceType));

    m_out.appendTo(isNotCell);
    join.add(boolean(false));
    finishInlineCall(call, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
