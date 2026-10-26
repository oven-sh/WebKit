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
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::NewError, { word32(static_cast<int32_t>(kind)), message, options }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
