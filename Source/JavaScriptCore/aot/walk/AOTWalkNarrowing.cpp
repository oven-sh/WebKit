/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTSweep.h"
#include "AOTTypeNarrowing.h"
#include <algorithm>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

bool buildCheckThatTypesPass(Walker& walker, const BodyValue& check, const InputsOfTypedOperation& inputs)
{
    if (check.opcode() != Opcode::RequireObjectCoercible || !walker.has(Mechanism::TypedOperations))
        return false;
    ValueID operand = check.operand(OperandIndex::RequireObjectCoercible::value);
    TypeID type = operand ? inputs.typeOf(operand) : TypeID();
    if (!type || isNeverType(type))
        return false;
    Type checked = walker.types().at(type);
    if (!checked.isJavaScriptValue() || (checked.primitives & (Primitive::UndefinedOrNull | Primitive::Absent | Primitive::Unspecified)))
        return false;
    if (inputs.areOnlyTypes())
        return true;
    walker.bind(check.id(), walker.referenceOf(operand));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
