/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubHelpers.h"
#include "AOTStubsConstruct.h"
#include <algorithm>
#include <initializer_list>
#include <optional>

namespace JSC { namespace AOT {

constexpr StubKey callValue(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValue).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callValueWithoutThis(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueWithoutThis).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callValueCell(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueCell).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callValueCellWithoutThis(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueCellWithoutThis).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callValueFunctionOfEngine(unsigned count, CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueFunctionOfEngine).withCount(count).withForm(static_cast<uint8_t>(position)); }
constexpr unsigned formOfDelegatedCallValue = descriptorOf(StubFamily::CallValue).numberOfForms - 1;
constexpr unsigned numberOfCallValueCopies = formOfDelegatedCallValue - 1;
constexpr bool isCallValueCopy(StubKey key) { return key.form() > static_cast<uint8_t>(CallPosition::Tail); }
constexpr StubKey callValueCopy(StubKey ordinary, unsigned copy) { return ordinary.withForm(copy ? static_cast<uint8_t>(CallPosition::Tail) + copy : static_cast<uint8_t>(CallPosition::Ordinary)); }
constexpr StubKey callValueDelegated(StubKey ordinary) { return ordinary.withForm(static_cast<uint8_t>(formOfDelegatedCallValue)); }
constexpr unsigned numberOfSetsOfLentArguments = 3;
constexpr StubKey callValueLending(unsigned count, unsigned lentArguments, CallPosition position = CallPosition::Ordinary) { return lentArguments ? StubKey(StubFamily::CallValueLending).withCount(count).withForm(static_cast<uint8_t>(lentArguments - 1 + numberOfSetsOfLentArguments * static_cast<unsigned>(position))) : callValue(count, position); }
constexpr StubKey callValueWithCount(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallValueWithCount).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callCellWithCount(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallCellWithCount).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callNotClosureWithCount(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallNotClosureWithCount).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey callBoundFunction(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallBoundFunction).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey constructValue(ConstructSyntax syntax) { return StubKey(StubFamily::ConstructValue).withForm(static_cast<uint8_t>(syntax)); }
constexpr StubKey resolveCallValue(unsigned count) { return StubKey(StubFamily::ResolveCallValue).withCount(std::min(count, numberOfArgumentGPRs + 1)); }
constexpr StubKey callEngineCallee() { return StubKey(StubFamily::CallEngineCallee); }
constexpr StubKey callEngineFunction() { return StubKey(StubFamily::CallEngineFunction); }

static constexpr unsigned maxArgumentsOfBoundCall = 4096;

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

void loadClosureWordOfThisRealm(StubAssembler&, GPRReg closure, const CallRegisters&, JumpList& isNotClosureOfThisRealm);
Jump branchUnlessWordBorrows(StubAssembler&, GPRReg word, GPRReg scratch, unsigned lentArguments);

void emitFillMissingArgumentRegisters(StubAssembler&, unsigned count);
void emitFillMissingArgumentRegisters(StubAssembler&, GPRReg count, GPRReg scratch);

void emitEnterClosure(StubAssembler&, const CallRegisters&, std::optional<unsigned> count, JumpList& isNotCallable);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
