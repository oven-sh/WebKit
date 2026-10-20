/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubFrontEnds.h"

namespace JSC { namespace AOT {

constexpr StubKey writeBarrier() { return StubKey(StubFamily::WriteBarrier); }
constexpr StubKey rememberElement() { return StubKey(StubFamily::RememberElement); }

PreludeExit emitBarrierPrelude(StubAssembler&);

static_assert(exists(writeBarrier()) && exists(rememberElement()));
static_assert(!exists(rememberElement().withCallerFrame(CallerFrame::Frameless)) && !exists(writeBarrier().withCount(1)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
