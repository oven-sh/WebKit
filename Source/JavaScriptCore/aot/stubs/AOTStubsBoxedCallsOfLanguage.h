/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsBoxedCalls.h"

namespace JSC { namespace AOT {

void emitStringIsWellFormed(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
void emitStringSearchFromPosition(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
bool searchesFromPosition(unsigned count);

bool pushesSeveralValues(unsigned count);
void emitArrayPushOfSeveral(StubAssembler&);

void emitCallOfBooleanConstructor(StubAssembler&, GPRReg scratch);
void emitCallOfNumberOrStringConstructor(StubAssembler&, GPRReg scratch);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
