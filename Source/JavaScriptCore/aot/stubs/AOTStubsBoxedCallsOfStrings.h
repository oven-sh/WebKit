/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsBoxedCalls.h"

namespace JSC { namespace AOT {

// For boxedCallHandlers[] of AOTStubsBoxedCalls.cpp. The receiver is a string. Each answers for every int32 position, inside the string or not, and goes on to the host function with the registers of the call as they were.
// charCodeAt, codePointAt. A rope is left to the host function, which makes it flat.
void emitStringCodeUnitAt(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
// charAt, at.
void emitStringCharacterAt(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
// slice, substring, substr.
void emitStringPiece(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
// lastIndexOf of a string, without a position.
void emitStringLastIndexOf(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
