/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"

namespace JSC { namespace AOT {

constexpr StubKey pollAtPrologue(bool functionMayCollect) { return StubKey(functionMayCollect ? StubFamily::PollAtPrologue : StubFamily::PollAtPrologueWithoutCollecting); }
constexpr bool isPollAtPrologue(StubKey key) { return key.family() == StubFamily::PollAtPrologue || key.family() == StubFamily::PollAtPrologueWithoutCollecting; }
constexpr StubKey pollAtLoop(CallerFrame callerFrame) { return StubKey(StubFamily::PollAtLoop).withCallerFrame(callerFrame); }
enum class PollBeforeEnteringForm : uint8_t { Plain, FunctionObject };
constexpr StubKey pollBeforeEntering(PollBeforeEnteringForm form) { return StubKey(StubFamily::PollBeforeEntering).withForm(static_cast<uint8_t>(form)); }

constexpr int32_t pollAtPrologueMustThrow = 1;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
