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

constexpr StubKey newClosure(unsigned captures) { return StubKey(StubFamily::NewClosure).withCount(captures); }
constexpr StubKey newSlimClosure(unsigned slimClosureKind, unsigned captures) { return StubKey(StubFamily::NewSlimClosure).withForm(slimClosureKind).withCount(captures); }
constexpr StubKey newSlimClosureInFrame(unsigned slimClosureKind, unsigned captures) { return StubKey(StubFamily::NewSlimClosureInFrame).withForm(slimClosureKind).withCount(captures); }
constexpr StubKey makeClosureOfField(unsigned slimClosureKind, bool capturesObject) { return StubKey(StubFamily::MakeClosureOfField).withForm(slimClosureKind).withCount(capturesObject); }
constexpr StubKey makeClosureOfFieldOverEnvironment(unsigned slimClosureKind) { return StubKey(StubFamily::MakeClosureOfField).withForm(slimClosureKind).withCount(2); }
constexpr StubKey closureOfTable() { return StubKey(StubFamily::ClosureOfTable); }
enum class ChangedClosure : uint8_t { Slim, Full };
constexpr StubKey changedClosureHasNumber(ChangedClosure form) { return StubKey(StubFamily::ChangedClosureHasNumber).withForm(static_cast<uint8_t>(form)); }
constexpr StubKey numberOfChangedClosure() { return StubKey(StubFamily::NumberOfChangedClosure); }
constexpr StubKey checkProofOfClosure() { return StubKey(StubFamily::CheckProofOfClosure); }
inline constexpr uint32_t noClosureNumber = UINT32_MAX;

void loadSlimClosureWord(StubAssembler&, GPRReg closure, GPRReg word);

static_assert(exists(newClosure(0)) && exists(newClosure(ClosureRequest::maxNumberOfCaptures)) && !exists(newClosure(ClosureRequest::maxNumberOfCaptures + 1)));
static_assert(exists(newSlimClosure(0, 0)) && exists(newSlimClosure(numberOfSlimClosureKinds - 1, ClosureRequest::maxSlimClosureCaptures)));
static_assert(!exists(newSlimClosure(numberOfSlimClosureKinds, 0)) && !exists(newSlimClosure(0, ClosureRequest::maxSlimClosureCaptures + 1)));
static_assert(exists(newSlimClosureInFrame(0, 0)) && exists(newSlimClosureInFrame(numberOfSlimClosureKinds - 1, ClosureRequest::maxSlimClosureCaptures)));
static_assert(!exists(newSlimClosureInFrame(numberOfSlimClosureKinds, 0)) && !exists(newSlimClosureInFrame(0, ClosureRequest::maxSlimClosureCaptures + 1)));
static_assert(exists(makeClosureOfField(0, false)) && exists(makeClosureOfField(numberOfSlimClosureKinds - 1, true)) && exists(makeClosureOfFieldOverEnvironment(numberOfSlimClosureKinds - 1)) && !exists(makeClosureOfField(0, true).withCount(3)));
static_assert(exists(changedClosureHasNumber(ChangedClosure::Full).withOperandRegister(allocatableGPRs.back())) && exists(numberOfChangedClosure().withOperandRegister(allocatableGPRs.back())) && !exists(changedClosureHasNumber(ChangedClosure::Slim).withOperandRegister(scratchImmediateGPR)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
