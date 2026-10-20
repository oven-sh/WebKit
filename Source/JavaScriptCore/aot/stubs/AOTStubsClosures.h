/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionData.h"
#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

// A function object. The operand is a ClosureRequest.
constexpr StubKey newClosure(unsigned captures) { return StubKey(StubFamily::NewClosure).withCount(captures); }
// The operand is the number of the function.
constexpr StubKey newSlimClosure(unsigned slimClosureKind, unsigned captures) { return StubKey(StubFamily::NewSlimClosure).withForm(slimClosureKind).withCount(captures); }
constexpr StubKey initializeClosure(unsigned captures) { return StubKey(StubFamily::InitializeClosure).withCount(captures); }
constexpr StubKey materializeEnvironment() { return StubKey(StubFamily::MaterializeEnvironment); }
constexpr StubKey closureEntry() { return StubKey(StubFamily::ClosureEntry); }

// closure is a slim closure, with or without properties.
void loadSlimClosureWord(StubAssembler&, GPRReg closure, GPRReg word);

static_assert(exists(newClosure(0)) && exists(newClosure(ClosureRequest::maxNumberOfCaptures)) && !exists(newClosure(ClosureRequest::maxNumberOfCaptures + 1)));
static_assert(exists(newSlimClosure(0, 0)) && exists(newSlimClosure(numberOfSlimClosureKinds - 1, ClosureRequest::maxSlimClosureCaptures)));
static_assert(!exists(newSlimClosure(numberOfSlimClosureKinds, 0)) && !exists(newSlimClosure(0, ClosureRequest::maxSlimClosureCaptures + 1)));
static_assert(exists(initializeClosure(ClosureRequest::maxSlimClosureCaptures)) && exists(closureEntry()));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
