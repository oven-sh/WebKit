/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsPropertyProbes.h"

namespace JSC { namespace AOT {

StubSignature signatureOfKeyedOperands(unsigned numberOfOperands, StubResult);
void loadNameOfKey(StubAssembler&, GPRReg key, GPRReg uid, GPRReg scratch, JumpList& hasNoName);
void callOperationOnOperandsAndReturn(StubAssembler&, Entry, unsigned numberOfOperands, OperationCall::Returns);
void callLeafLookup(StubAssembler&, Entry, GPRReg second, std::optional<GPRReg> value, GPRReg baseAfterCall);

static_assert(firstStubOperandRegister == StubRegister::R0 && keyCellRegister == StubRegister::A1 && storedValueByValRegister == StubRegister::A2);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
