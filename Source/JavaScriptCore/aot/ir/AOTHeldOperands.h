/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

template<HoldsOpcode Operation>
uint32_t numberOfHeldOperandsOf(const Operation& operation)
{
    Opcode opcode = opcodeOf(operation);
    if (VariadicOperandPosition::held(opcode))
        return static_cast<uint32_t>(operation.variadicOperands().size());
    if (PayloadWord::numberOfHeld(opcode))
        return operation.numberOfHeld().value_or(0);
    return 0;
}

template<HoldsOpcode Operation>
auto heldOperandsOf(const Operation& operation LIFETIME_BOUND)
{
    return operation.variadicOperands().last(numberOfHeldOperandsOf(operation));
}

template<HoldsOpcode Operation>
bool isHeldOperand(const Operation& user, uint32_t operandIndex)
{
    return operandIndex + numberOfHeldOperandsOf(user) >= numberOfNamedOperandsOf(opcodeOf(user)) + user.variadicOperands().size();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
