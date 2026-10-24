/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// f.call(t, a, b), f.apply(t, list) and Reflect.apply(f, t, list) call one operand once and return what it returns, and do nothing else.
struct ForwardedCallPlan {
    ValueID callee;
    ValueID thisValue;
    ValueID list;
    uint32_t firstForwardedArgument { 0 };
};

bool forwardsCall(const BuiltinOverload&);
// A call by value says of a callee that cannot be called what `call` and `apply` say of such a receiver. Reflect.apply has words of its own.
bool forwardsCallOfAnyReceiver(const BuiltinOverload&);

// The plan has no list when the call passes none, or one that is always undefined or null.
std::optional<ForwardedCallPlan> planForwardedCall(const Program&, const Function&, const BuiltinCall&);
// The operand that an overload which passes the call on calls.
std::optional<BuiltinOperand> operandCalledBy(const BuiltinOverload&);

bool isAlwaysArray(const Type&);

// The list of the plan is always an array.
void becomeForwardedCall(Value& call, const ForwardedCallPlan&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
