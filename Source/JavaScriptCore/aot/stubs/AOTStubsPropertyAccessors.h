/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsPropertyProbes.h"

namespace JSC { namespace AOT {

void callGetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg getterSetter, JumpList& cannot);
void callGetterFunctionAndReturn(StubAssembler&, GPRReg receiver, GPRReg getter, JumpList& cannot);
void callSetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg value, GPRReg getterSetter, JumpList& cannot);
void callCustomGetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg accessorIndex, GPRReg uid, std::optional<GPRReg> keyCell);
void callCustomSetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg value, GPRReg accessorIndex, GPRReg uid, std::optional<GPRReg> keyCell);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
