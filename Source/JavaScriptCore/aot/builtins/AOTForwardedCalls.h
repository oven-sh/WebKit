/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

struct ForwardedCallPlan {
    ValueID callee;
    ValueID thisValue;
    ValueID list;
    uint32_t firstForwardedArgument { 0 };
};

bool forwardsCall(const BuiltinOverload&);
bool forwardsCallOfAnyReceiver(const BuiltinOverload&);

std::optional<ForwardedCallPlan> planForwardedCall(const Program&, const Function&, const BuiltinCall&);
std::optional<BuiltinOperand> operandCalledBy(const BuiltinOverload&);

bool isAlwaysArray(const Type&);

void becomeForwardedCall(Value& call, const ForwardedCallPlan&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
