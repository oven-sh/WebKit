/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsPropertyProbes.h"

namespace JSC { namespace AOT {

void loadNameOfKey(StubAssembler&, GPRReg key, GPRReg uid, GPRReg scratch, JumpList& hasNoName);
// In a stub that keeps the registers of its caller, lostAnyway names those it need not keep.
void callLeafLookup(StubAssembler&, Entry, GPRReg second, std::optional<GPRReg> value, GPRReg baseAfterCall, const RegisterSet& lostAnyway = { });
// A slot for reads has said that nothing is found under the key. Its header names the Structure.
void rememberAbsenceOfKey(StubAssembler&, GPRReg key, GPRReg header, GPRReg place, GPRReg scratch);

static_assert(firstStubOperandRegister == StubRegister::R0 && keyCellRegister == StubRegister::A1 && storedValueByValRegister == StubRegister::A2);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
