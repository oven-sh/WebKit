/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionData.h"
#include "AOTStubDescriptor.h"
#include "AOTStubHelpers.h"

namespace JSC { namespace AOT {

// A function object. The operand is a ClosureRequest.
constexpr StubKey newClosure(unsigned captures) { return StubKey(StubFamily::NewClosure).withCount(captures); }
// The operand is the number of the function.
constexpr StubKey newSlimClosure(unsigned slimClosureKind, unsigned captures) { return StubKey(StubFamily::NewSlimClosure).withForm(slimClosureKind).withCount(captures); }
// Creates the closure that a field deferred, capturing the object that has the field or nothing, and stores it on the first read of the slot.
// The first operand holds the function number in its lower half and the slot offset in its upper half. The second operand is the object.
constexpr StubKey makeClosureOfField(unsigned slimClosureKind, bool capturesObject) { return StubKey(StubFamily::MakeClosureOfField).withForm(slimClosureKind).withCount(capturesObject); }
// The same for a closure over the environment that a hidden field of the object keeps. The slot offset has 16 bits here, and the 16 bits above it are the offset of the hidden field.
constexpr StubKey makeClosureOfFieldOverEnvironment(unsigned slimClosureKind) { return StubKey(StubFamily::MakeClosureOfField).withForm(slimClosureKind).withCount(2); }
// The closure that the closure table holds for a function without captures, made at the first request. The immediate is the number of the generic entrypoint.
constexpr StubKey closureOfTable() { return StubKey(StubFamily::ClosureOfTable); }
constexpr StubKey initializeClosure(unsigned captures) { return StubKey(StubFamily::InitializeClosure).withCount(captures); }
constexpr StubKey materializeEnvironment() { return StubKey(StubFamily::MaterializeEnvironment); }
// Takes any value but the empty one, and the upper half of the closure word of the function.
constexpr StubKey valueIsClosureOf() { return StubKey(StubFamily::ValueIsClosureOf); }
// Takes any value but the empty one. True of a closure whose word says so: ClosureWord::endsItsTailCallsBit.
constexpr StubKey valueEndsItsTailCalls() { return StubKey(StubFamily::ValueEndsItsTailCalls); }
// Takes the function object that the caller was entered with. The frame record of the caller lies right below the stack pointer of its own caller.
constexpr StubKey recordCallee() { return StubKey(StubFamily::RecordCallee); }

// closure is a slim closure, with or without properties.
// isOfOtherImage: for whoever does not know that the closure is of this image. The words of this image say nothing under the number of another.
void loadSlimClosureWord(StubAssembler&, GPRReg closure, GPRReg word, JumpList* isRenamed = nullptr, JumpList* isOfOtherImage = nullptr);

static_assert(exists(newClosure(0)) && exists(newClosure(ClosureRequest::maxNumberOfCaptures)) && !exists(newClosure(ClosureRequest::maxNumberOfCaptures + 1)));
static_assert(exists(newSlimClosure(0, 0)) && exists(newSlimClosure(numberOfSlimClosureKinds - 1, ClosureRequest::maxSlimClosureCaptures)));
static_assert(!exists(newSlimClosure(numberOfSlimClosureKinds, 0)) && !exists(newSlimClosure(0, ClosureRequest::maxSlimClosureCaptures + 1)));
static_assert(exists(makeClosureOfField(0, false)) && exists(makeClosureOfField(numberOfSlimClosureKinds - 1, true)) && exists(makeClosureOfFieldOverEnvironment(numberOfSlimClosureKinds - 1)) && !exists(makeClosureOfField(0, true).withCount(3)));
static_assert(exists(initializeClosure(ClosureRequest::maxSlimClosureCaptures)) && exists(valueIsClosureOf()));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
