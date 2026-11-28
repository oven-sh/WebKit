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
constexpr StubKey enterClosureWithLoadedArguments() { return StubKey(StubFamily::EnterClosureWithLoadedArguments); }
constexpr StubKey enterClosureForConstruct() { return StubKey(StubFamily::EnterClosureForConstruct); }
constexpr StubKey startModule() { return StubKey(StubFamily::StartModule); }
constexpr StubKey startModuleThatRunTimeNames() { return StubKey(StubFamily::StartModuleThatRunTimeNames); }

void emitEnterAdapter(StubAssembler&, unsigned extraBytes = 0);
static constexpr unsigned adapterArgumentCountWithOutOfLineArguments = numberOfArgumentGPRs + 2;
static constexpr unsigned outOfLineArgumentCountTagShift = 48;
static constexpr uint16_t outOfLineArgumentCountTag = 1;
static_assert(!((static_cast<uint64_t>(outOfLineArgumentCountTag) << outOfLineArgumentCountTagShift) & JSValue::NumberTag));
void emitAdapterEpilogue(StubAssembler&);
void emitRestoreAdapterCallerRegisters(StubAssembler&, GPRReg adapterFrame);

void emitConstructWithPlainFunction(StubAssembler&, GPRReg word, CCallHelpers::JumpList& isNotPlainFunction);
CCallHelpers::Address emitLocateRememberedConstructWord(StubAssembler&, GPRReg wordOfCall, GPRReg entry);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
