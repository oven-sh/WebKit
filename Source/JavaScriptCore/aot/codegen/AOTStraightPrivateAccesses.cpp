/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTSlotPlan.h"
#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfPrivateAccesses()
{
    return { Opcode::PrivateGet, Opcode::PrivateSet, Opcode::PrivateDefine, Opcode::PrivateHas };
}

void StraightEmitter::generatePrivateAccess(const BodyValue& value)
{
    std::optional<uint16_t> field = plan().slot(value.layout(), value.field()).indexInLayoutRecord;
    if (!field) {
        fail(value);
        return;
    }
    ValueID object = value.objectOperand();
    ValueID token = value.tokenOperand();
    switch (value.opcode()) {
    case Opcode::PrivateGet:
        callStub(value, privateGet(), { object, token }, { .immediate = *field });
        define(value);
        return;
    case Opcode::PrivateHas:
        callOperation(value, RuntimeOperation::PrivateHas, { object, token, StraightArgument::word(*field) });
        define(value);
        return;
    case Opcode::PrivateSet:
        callOperation(value, RuntimeOperation::PrivateSet, { object, token, StraightArgument::word(*field), value.valueOperand() });
        return;
    case Opcode::PrivateDefine:
        callOperation(value, RuntimeOperation::PrivateDefine, { object, token, StraightArgument::word(*field), value.valueOperand() });
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
