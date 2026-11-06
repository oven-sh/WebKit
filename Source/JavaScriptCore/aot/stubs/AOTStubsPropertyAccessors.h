/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsPropertyProbes.h"

namespace JSC { namespace AOT {

// Each ends the stub on every path but its list, where receiver, closure, value and getterSetter are intact.
void callGetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg getterSetter, JumpList& cannot);
// getter is T11.
void callGetterFunctionAndReturn(StubAssembler&, GPRReg receiver, GPRReg getter, JumpList& cannot);
void callSetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg value, GPRReg getterSetter, JumpList& cannot);
// The name of a key belongs to the cell of the key.
void callCustomGetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg accessorIndex, GPRReg uid, std::optional<GPRReg> keyCell);
void callCustomSetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg value, GPRReg accessorIndex, GPRReg uid, std::optional<GPRReg> keyCell);
void readClosureLengthAndReturn(StubAssembler&, GPRReg closure, JumpList& miss);
void readClosureNameAndReturn(StubAssembler&, GPRReg closure, JumpList& miss);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
