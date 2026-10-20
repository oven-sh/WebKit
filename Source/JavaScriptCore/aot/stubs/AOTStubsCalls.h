/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubHelpers.h"
#include "AOTStubsConstruct.h"
#include <initializer_list>
#include <optional>

namespace JSC { namespace AOT {

constexpr StubKey callValue(unsigned count) { return StubKey(StubFamily::CallValue).withCount(count); }
constexpr StubKey callValueWithoutThis(unsigned count) { return StubKey(StubFamily::CallValueWithoutThis).withCount(count); }
constexpr StubKey callValueCell(unsigned count) { return StubKey(StubFamily::CallValueCell).withCount(count); }
constexpr StubKey callValueNotClosure(unsigned count) { return StubKey(StubFamily::CallValueNotClosure).withCount(count); }
constexpr StubKey callValueNamed(unsigned count) { return StubKey(StubFamily::CallValueNamed).withCount(count); }
constexpr StubKey callValueWithCount() { return StubKey(StubFamily::CallValueWithCount); }
constexpr StubKey callCellWithCount() { return StubKey(StubFamily::CallCellWithCount); }
// The first argument is new.target. The two forms differ in what a callee that is no function of the program is handed to.
constexpr StubKey constructValue(ConstructSyntax syntax) { return StubKey(StubFamily::ConstructValue).withForm(static_cast<uint8_t>(syntax)); }
constexpr StubKey resolveCallValue(unsigned count) { return StubKey(StubFamily::ResolveCallValue).withCount(count); }
constexpr StubKey callResolvedValue() { return StubKey(StubFamily::CallResolvedValue); }
constexpr StubKey callBoundTarget() { return StubKey(StubFamily::CallBoundTarget); }
constexpr StubKey callEngineCallee() { return StubKey(StubFamily::CallEngineCallee); }

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

void emitFillMissingArgumentRegisters(StubAssembler&, unsigned count);
void emitFillMissingArgumentRegisters(StubAssembler&, GPRReg count, GPRReg scratch);

void emitEnterClosure(StubAssembler&, const CallRegisters&, std::optional<unsigned> count, JumpList& isNotCallable);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
