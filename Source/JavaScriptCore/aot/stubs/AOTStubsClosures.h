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
// The operand is the closure number of the function.
constexpr StubKey newSlimClosure(unsigned slimClosureKind, unsigned captures) { return StubKey(StubFamily::NewSlimClosure).withForm(slimClosureKind).withCount(captures); }
// Makes the closure that a field deferred, which captures the object that has the field or nothing, and stores it in the slot: for the first read of the slot.
// The first operand holds the closure number of the function in its lower half and the offset of the slot in its upper half. The second operand is the object.
constexpr StubKey makeClosureOfField(unsigned slimClosureKind, bool capturesObject) { return StubKey(StubFamily::MakeClosureOfField).withForm(slimClosureKind).withCount(capturesObject); }
// The same for a closure over the environment that a hidden field of the object keeps. The offset of the slot has 16 bits here, and the 16 bits above it are the offset of the hidden field.
constexpr StubKey makeClosureOfFieldOverEnvironment(unsigned slimClosureKind) { return StubKey(StubFamily::MakeClosureOfField).withForm(slimClosureKind).withCount(2); }
// The closure that the closure table holds for a function without captures, made at the first request. The immediate is the closure number of the function.
constexpr StubKey closureOfTable() { return StubKey(StubFamily::ClosureOfTable); }
// What a test for a closure asks of a cell that is not as a closure of the function is created: a slim closure that has got a butterfly, a function object with another Structure, or no closure at all.
// Whether it is a closure of that form, of this instance, with the closure number in the immediate. No register that compiled code keeps a value in is written.
enum class ChangedClosure : uint8_t { Slim, Full };
constexpr StubKey changedClosureHasNumber(ChangedClosure form) { return StubKey(StubFamily::ChangedClosureHasNumber).withForm(static_cast<uint8_t>(form)); }
// The closure number of a closure of either form. noClosureNumber, which lies above all of them, where the cell is no closure of this instance.
constexpr StubKey numberOfChangedClosure() { return StubKey(StubFamily::NumberOfChangedClosure); }
inline constexpr uint32_t noClosureNumber = UINT32_MAX;

// closure is a slim closure, with or without properties.
void loadSlimClosureWord(StubAssembler&, GPRReg closure, GPRReg word);

static_assert(exists(newClosure(0)) && exists(newClosure(ClosureRequest::maxNumberOfCaptures)) && !exists(newClosure(ClosureRequest::maxNumberOfCaptures + 1)));
static_assert(exists(newSlimClosure(0, 0)) && exists(newSlimClosure(numberOfSlimClosureKinds - 1, ClosureRequest::maxSlimClosureCaptures)));
static_assert(!exists(newSlimClosure(numberOfSlimClosureKinds, 0)) && !exists(newSlimClosure(0, ClosureRequest::maxSlimClosureCaptures + 1)));
static_assert(exists(makeClosureOfField(0, false)) && exists(makeClosureOfField(numberOfSlimClosureKinds - 1, true)) && exists(makeClosureOfFieldOverEnvironment(numberOfSlimClosureKinds - 1)) && !exists(makeClosureOfField(0, true).withCount(3)));
static_assert(exists(changedClosureHasNumber(ChangedClosure::Full).withOperandRegister(allocatableGPRs.back())) && exists(numberOfChangedClosure().withOperandRegister(allocatableGPRs.back())) && !exists(changedClosureHasNumber(ChangedClosure::Slim).withOperandRegister(scratchImmediateGPR)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
