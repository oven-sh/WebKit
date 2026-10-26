/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"

namespace JSC { namespace AOT {

constexpr StubKey pollAtPrologue() { return StubKey(StubFamily::PollAtPrologue); }
constexpr StubKey pollAtLoop(CallerFrame callerFrame) { return StubKey(StubFamily::PollAtLoop).withCallerFrame(callerFrame); }
// FunctionObject: the stub that polls jumps into the code of the function object in calleeGPR next, with the stack as it is.
enum class PollBeforeEnteringForm : uint8_t { Plain, FunctionObject };
constexpr StubKey pollBeforeEntering(PollBeforeEnteringForm form) { return StubKey(StubFamily::PollBeforeEntering).withForm(static_cast<uint8_t>(form)); }

constexpr int32_t pollAtPrologueMustThrow = 1;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
