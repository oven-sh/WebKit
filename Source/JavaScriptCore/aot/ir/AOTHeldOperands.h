/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

// The last operands of a suspension are what the suspended function holds. Nothing reads them: the saved frame lists them as references.
inline uint32_t numberOfHeldOperandsOf(const Value& value)
{
    if (VariadicOperandPosition::held(value.opcode))
        return value.numberOfVariadicOperands();
    if (Value::opcodesWithNumberOfHeld.contains(value.opcode))
        return value.numberOfHeld().value_or(0);
    return 0;
}

inline std::span<const ValueID> heldOperandsOf(const Value& value LIFETIME_BOUND)
{
    return value.variadicOperands().last(numberOfHeldOperandsOf(value));
}

inline bool isHeldOperand(const Value& user, uint32_t operandIndex)
{
    return operandIndex + numberOfHeldOperandsOf(user) >= user.numberOfOperands();
}

inline bool canHoldOperands(Opcode opcode)
{
    return VariadicOperandPosition::held(opcode) || Value::opcodesWithNumberOfHeld.contains(opcode);
}

inline void setHeldOperands(Value& suspension, std::span<const ValueID> held)
{
    RELEASE_ASSERT(canHoldOperands(suspension.opcode));
    Vector<ValueID, 8> operands(suspension.variadicOperands().first(suspension.numberOfVariadicOperands() - numberOfHeldOperandsOf(suspension)));
    operands.append(held);
    suspension.setVariadicOperands(operands.span());
    if (Value::opcodesWithNumberOfHeld.contains(suspension.opcode))
        suspension.setNumberOfHeld(held.empty() ? std::nullopt : std::optional<uint32_t>(held.size()));
}

// Only the end of an object or of a symbol can be observed.
inline bool mayHoldObject(const Type& type)
{
    return type.environment || (type.isJavaScriptValue() && (type.hasObjects() || type.primitives & Primitive::Symbol));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
