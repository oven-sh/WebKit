/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"
#include "CCallHelpers.h"

namespace JSC { namespace AOT {

class StubAssembler;

constexpr StubKey enterClosure() { return StubKey(StubFamily::EnterClosure); }
constexpr StubKey enterSlimClosure() { return StubKey(StubFamily::EnterSlimClosure); }
constexpr StubKey enterModuleBody() { return StubKey(StubFamily::EnterModuleBody); }
constexpr StubKey enterClosureForConstruct() { return StubKey(StubFamily::EnterClosureForConstruct); }

void emitEnterAdapter(StubAssembler&, GPRReg calleeScratch, unsigned extraBytes = 0);
void emitAdapterEpilogue(StubAssembler&);
void emitRestoreAdapterCallerRegisters(StubAssembler&, GPRReg adapterFrame);

// The callee is a closure of this realm that can be called, and word is its word.
void emitConstructWithPlainFunction(StubAssembler&, GPRReg word, CCallHelpers::JumpList& isNotPlainFunction);
// Where ConstructWordCache has the function of a word of [[Call]].
CCallHelpers::Address emitLocateRememberedConstructWord(StubAssembler&, GPRReg wordOfCall, GPRReg entry);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
