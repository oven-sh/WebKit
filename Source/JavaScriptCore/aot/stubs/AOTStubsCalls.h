/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubHelpers.h"
#include "AOTStubsConstruct.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <initializer_list>
#include <optional>

namespace JSC { namespace AOT {

constexpr StubKey callValue(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValue).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callValueWithoutThis(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueWithoutThis).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callValueCell(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueCell).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callValueCellWithoutThis(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueCellWithoutThis).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callValueNotClosure(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueNotClosure).withCount(count).withForm(static_cast<uint8_t>(position)); }
// The callee is one that callEngineCallee() calls.
constexpr StubKey callValueEngineCallee(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueEngineCallee).withCount(count).withForm(static_cast<uint8_t>(position)); }
// Bit i of lentArguments: argument i is a slim closure that lies in the frame of the caller and that nothing else uses. A callee whose closure word does not say that it borrows all of them gets copies on the heap.
constexpr unsigned numberOfSetsOfLentArguments = 3;
constexpr StubKey callValueLending(unsigned count, unsigned lentArguments, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueLending).withCount(count).withForm(static_cast<uint8_t>(lentArguments - 1 + numberOfSetsOfLentArguments * static_cast<unsigned>(position))); }
constexpr StubKey callValueWithCount(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueWithCount).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callCellWithCount(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallCellWithCount).withForm(static_cast<uint8_t>(position)); }
// The callee is anything but a closure that the caller can enter.
constexpr StubKey callNotClosureWithCount(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallNotClosureWithCount).withForm(static_cast<uint8_t>(position)); }
// The callee is a JSBoundFunction, whatever its structure.
constexpr StubKey callBoundFunction(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallBoundFunction).withForm(static_cast<uint8_t>(position)); }
// The first argument is new.target. The two forms differ in what a callee that is no function of the program is handed to.
constexpr StubKey constructValue(ConstructSyntax syntax) { return StubKey(StubFamily::ConstructValue).withForm(static_cast<uint8_t>(syntax)); }
// One stub resolves every call that passes more arguments than there are registers: such a call passes its count as well.
constexpr StubKey resolveCallValue(unsigned count) { return StubKey(StubFamily::ResolveCallValue).withCount(std::min(count, numberOfArgumentGPRs + 1)); }
constexpr StubKey callResolvedValue() { return StubKey(StubFamily::CallResolvedValue); }
constexpr StubKey callEngineCallee() { return StubKey(StubFamily::CallEngineCallee); }
// The callee is a JSFunction: nothing has to ask whether it can be called.
constexpr StubKey callEngineFunction() { return StubKey(StubFamily::CallEngineFunction); }
// Jumped to where a function would return, with a result that is not of the type that the function promises.
constexpr StubKey returnMisfit() { return StubKey(StubFamily::ReturnMisfit); }

// Bound and passed arguments together. A longer list is not copied: the call is the engine's.
AOT_THRESHOLD(unsigned, maxArgumentsOfBoundCall, 4096);

inline constexpr StubRegister closureWordRegister = StubRegister::T11;
#if CPU(X86_64)
inline constexpr StubRegister callTargetRegister = StubRegister::T11;
inline constexpr StubRegister callTemporaryRegister = StubRegister::T13;
#else
inline constexpr StubRegister callTargetRegister = StubRegister::T12;
inline constexpr StubRegister callTemporaryRegister = StubRegister::T11;
#endif

struct CallRegisters {
    GPRReg word;
    GPRReg scratch;
    GPRReg target;
    GPRReg temporary;

    static CallRegisters allocate(StubAssembler&);
};

struct ClosureHeaderBranches {
    Jump isSlim;
    Jump isNeither;
};

StubSignature callConventionSignature(std::initializer_list<StubRegister> operands);
StubSignature callConventionSignatureWithCount();

ClosureHeaderBranches branchIfNotPlainClosureHeader(StubAssembler&, GPRReg structureID, GPRReg scratch);

// A closure that was renamed is entered like a closure of another realm: through the adapter, whose frame holds it.
enum class RenamedClosures : bool { AreEnteredThroughAdapter, AreLikeOthers };
void loadClosureWordOfThisRealm(StubAssembler&, GPRReg closure, const CallRegisters&, JumpList& isNotClosureOfThisRealm, RenamedClosures = RenamedClosures::AreEnteredThroughAdapter);

void emitFillMissingArgumentRegisters(StubAssembler&, unsigned count);
void emitFillMissingArgumentRegisters(StubAssembler&, GPRReg count, GPRReg scratch);

void emitEnterClosure(StubAssembler&, const CallRegisters&, std::optional<unsigned> count, JumpList& isNotCallable);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
