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
    ValueID newTarget;
    ValueID list;
    std::optional<BuiltinOperand> operandOfList;
    uint32_t firstForwardedArgument { 0 };
    bool constructs { false };
    // A callee that cannot refuse needs no name, and what it calls in its turn has none at the place.
    bool calleeMayRefuse { true };
    // Function.prototype.call or apply. Its frame is listed when the callee refuses, unless the call is done in place.
    BuiltinID tailCallingBuiltin;
};

bool forwardsCall(const BuiltinOverload&);
bool forwardsCallOfAnyReceiver(BuiltinID);
inline bool forwardsCallOfAnyReceiver(const BuiltinOverload& overload) { return forwardsCallOfAnyReceiver(overload.builtin); }

std::optional<ForwardedCallPlan> planForwardedCall(const Program&, const Function&, const BuiltinCall&);
// The built-in refuses a callee and a new.target that are no constructors in its own words, and before it reads the list.
std::optional<ForwardedCallPlan> planForwardedConstruct(const Program&, const Function&, const BuiltinCall&);
// Written any other way than `Reflect.construct(..)`, the built-in has a frame of its own between the caller and the constructor. What may be no constructor is refused by the built-in, in its own words.
bool constructsInPlaceWithoutRefusal(const Program&, const Function&, const Value& builtinCall, const ForwardedCallPlan&);
std::optional<BuiltinOperand> operandCalledBy(const BuiltinOverload&);
std::optional<BuiltinOperand> operandListedBy(const BuiltinOverload&);

bool isAlwaysArray(const Type&);
// JSC compiles `f.call(..)`, `f.apply(..)` and the library's `f.$apply(..)` in place: Function.prototype.call or apply has no frame of its own. Only the first two name their callee.
bool forwardsInPlace(const Value& builtinCall);

void becomeForwardedCall(Value& call, const ForwardedCallPlan&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
