/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsBoxedCalls.h"

namespace JSC { namespace AOT {

void emitStringCodeUnitAt(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
void emitStringCharacterAt(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
void emitStringPiece(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
void emitStringLastIndexOf(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
void emitStringLocaleCompare(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
