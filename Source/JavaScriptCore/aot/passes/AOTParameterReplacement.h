/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FunctionPassContext;

struct ParameterPosition {
    ValueID parameter;
    BlockID block;
    uint32_t index { 0 };
};

size_t removeTrivialParameters(FunctionPassContext&, Vector<ParameterPosition>&& candidatesInReversePostOrder);

inline bool canReplaceParameter(const Function& function, ValueID argument, ValueID parameter)
{
    const Value& replacement = function.at(argument);
    const Value& replaced = function.at(parameter);
    return replacement.type == replaced.type && replacement.expected() == replaced.expected();
}

inline bool isCastOf(const Function& function, const ValueSubstitution& substitution, ValueID value, ValueID of)
{
    while (isCast(function.at(value).opcode)) {
        value = substitution.resolve(function.at(value).operand(0));
        if (value == of)
            return true;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
