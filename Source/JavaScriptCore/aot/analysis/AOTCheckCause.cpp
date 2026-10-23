/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCheckCause.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

Wire::CheckCause computeUnknownObjectCause(const Function& function, ValueID operand)
{
    const Value* definition = &function.at(sameObjectThroughCasts(function, operand));
    if (definition->opcode == Opcode::ResultAt)
        definition = &function.at(definition->resultsOperand());
    Opcode opcode = definition->opcode;

    switch (effectRuleOf(opcode)) {
    default:
        return Wire::CheckCause::UnknownCallee;
    case EffectRule::Access:
        if (familyOf(opcode) == OperationFamily::Access && !Value::opcodesWithKey.contains(opcode))
            return Wire::CheckCause::UnprovenContents;
        break;
    case EffectRule::Declared:
    case EffectRule::Operator:
        break;
    }
    if (familyOf(opcode) == OperationFamily::ElementAccess || maySuspend(opcode))
        return Wire::CheckCause::UnprovenContents;
    if (accessClassOf(opcode) == AccessClass::IndirectCall)
        return Wire::CheckCause::UnknownCallee;
    return Wire::CheckCause::UnknownObject;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
