/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"

namespace JSC { namespace AOT { namespace Typed {

class BodyIndex;
class BodyValue;
class Program;

// `function outer() { function a() { .. } function b() { .. } }`, where a closure names `a` or `b`: the function lies in a slot of the scope that declares it.
// The front end lays the slot down as the closures of that one function, which nothing assigns to. Null: the field is no such slot.
FunctionID functionDeclaredInScopeSlot(const Program&, LayoutID environment, uint32_t field);
// `env L` and nothing beside it, laid down by the front end. Null: the capture takes something else.
LayoutID scopeThatCaptureTakes(const Program&, FunctionID, uint32_t capture);
// A declared function is made where its scope is entered, over that scope, and stored into it. So the closure in the slot of an environment has captured that very environment.
// True: the PutEnvironmentField keeps to that, or stores into no such slot. BodyVerifier asks it of every one, and findCaptureAtHand() leans on it.
bool storeKeepsDeclaredFunctionOverItsOwnScope(const Program&, const BodyIndex&, const BodyValue& store);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
