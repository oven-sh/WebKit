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
constexpr StubKey pollBeforeEntering() { return StubKey(StubFamily::PollBeforeEntering); }

constexpr int32_t pollAtPrologueMustThrow = 1;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
