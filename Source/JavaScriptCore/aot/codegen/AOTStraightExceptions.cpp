/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "Exception.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfExceptions()
{
    return { Opcode::CatchValue, Opcode::CatchException };
}

void StraightEmitter::generateException(const BodyValue& value)
{
    BodyBlock block = currentBlock();
    ValueID exception;
    for (BodyValue other : block.values()) {
        if (other.opcode() == Opcode::CatchException)
            exception = other.id();
        if (other.opcode() != Opcode::CatchException && other.opcode() != Opcode::CatchValue)
            break;
    }
    switch (value.opcode()) {
    case Opcode::CatchException:
        if (block.firstValue() != value.id()) {
            fail(value);
            return;
        }
        callOperation(value, RuntimeOperation::CatchException, { }, { .followsThrow = true });
        define(value);
        return;
    case Opcode::CatchValue:
        if (exception) {
            if (exception.index() > value.id().index() || m_index.isUnused(exception)) {
                fail(value);
                return;
            }
            GPRReg result = loadIntoResultRegister(exception);
            jit().load64(CCallHelpers::Address(result, JSC::Exception::valueOffset()), result);
        } else
            callOperation(value, RuntimeOperation::Catch, { }, { .followsThrow = true });
        define(value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
