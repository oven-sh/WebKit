/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsBoxedCalls.h"

namespace JSC { namespace AOT {

// For boxedCallHandlers[] of AOTStubsBoxedCalls.cpp. The receiver is a string. Each goes on to the host function with the registers of the call as they were.
// isWellFormed. A flat string of Latin-1 characters has no surrogates. Every other string is left to the host function.
void emitStringIsWellFormed(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
// indexOf, lastIndexOf, startsWith of a string, without a position or with an int32, inside the string or not.
void emitStringSearchFromPosition(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
// For acceptsArgumentCount(): `text.indexOf(part, from)`.
bool searchesFromPosition(unsigned count);

// For acceptsArgumentCount(): `list.push(a, b)`.
bool pushesSeveralValues(unsigned count);
// For emitArrayPush(), behind its last test. The receiver is a list, and nothing is left to the host function.
void emitArrayPushOfSeveral(StubAssembler&);

// For generateCallInternalFunction(), in front of the frame. `list.filter(Boolean)`: where the callee is Boolean, of whichever realm, and the truth of the first argument takes no operation, the stub returns.
// Else it falls through with the registers of the call as they were.
void emitCallOfBooleanConstructor(StubAssembler&, GPRReg scratch);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
