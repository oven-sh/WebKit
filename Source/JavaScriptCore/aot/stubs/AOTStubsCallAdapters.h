/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTStubKey.h"
#include "CCallHelpers.h"

namespace JSC { namespace AOT {

class StubAssembler;

constexpr StubKey enterClosure() { return StubKey(StubFamily::EnterClosure); }
constexpr StubKey enterSlimClosure() { return StubKey(StubFamily::EnterSlimClosure); }
constexpr StubKey enterModuleBody() { return StubKey(StubFamily::EnterModuleBody); }
constexpr StubKey enterClosureForConstruct() { return StubKey(StubFamily::EnterClosureForConstruct); }

// Only native code enters for a job of a coroutine, with as many arguments as fill their frame.
enum class AdapterCaller : uint8_t { NativeCodeOrInterpreter, NativeCodeForJobOfCoroutine };
void emitEnterAdapter(StubAssembler&, AdapterCaller, unsigned extraBytes = 0);
// Native code passes a longer list out of line: the frame holds the register arguments and a pointer to all arguments, and the high word of its argument count is their number.
static constexpr unsigned adapterArgumentCountWithOutOfLineArguments = numberOfArgumentGPRs + 1;
void emitAdapterEpilogue(StubAssembler&);
void emitRestoreAdapterCallerRegisters(StubAssembler&, GPRReg adapterFrame);

// The callee is a closure of this realm that can be called, and word is its word.
void emitConstructWithPlainFunction(StubAssembler&, GPRReg word, CCallHelpers::JumpList& isNotPlainFunction);
// Where ConstructWordCache has the function of a word of [[Call]].
CCallHelpers::Address emitLocateRememberedConstructWord(StubAssembler&, GPRReg wordOfCall, GPRReg entry);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
