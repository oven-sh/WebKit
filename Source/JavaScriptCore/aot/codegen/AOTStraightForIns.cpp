/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfForIns()
{
    return { Opcode::ForInOpen, Opcode::ForInStep };
}

void StraightEmitter::generateForIn(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::ForInOpen:
        callOperation(value, RuntimeOperation::OpenEnumeration, { value.objectOperand() });
        define(value);
        return;
    case Opcode::ForInStep:
        callOperation(value, RuntimeOperation::StepEnumeration, { value.enumeratorOperand(), value.objectOperand() });
        defineResult(value, static_cast<uint32_t>(ForInStepResult::Done), resultOfCall());
        defineResult(value, static_cast<uint32_t>(ForInStepResult::Key), GPRInfo::returnValueGPR2);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
