/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsCallFrames.h"
#include "AOTStubsCalls.h"

namespace JSC { namespace AOT {

constexpr StubKey callWithArray() { return StubKey(StubFamily::CallWithArray); }
constexpr StubKey callWithList() { return StubKey(StubFamily::CallWithList); }
constexpr StubKey tailCallWithList() { return StubKey(StubFamily::TailCallWithList); }
constexpr StubKey superConstructWithList() { return StubKey(StubFamily::SuperConstructWithList); }
constexpr StubKey callFromArgumentListFrame() { return StubKey(StubFamily::CallFromArgumentListFrame); }
constexpr StubKey returnFromArgumentListFrame() { return StubKey(StubFamily::ReturnFromArgumentListFrame); }

constexpr unsigned maxArrayLengthCopiedToStack = 4096;

struct ArgumentList {
    GPRReg count;
    GPRReg words;
};

void emitPushArgumentListFrame(StubAssembler&, const ArgumentList&, const CallRegisters&, JumpList& doesNotFitOnStack);
void emitLoadArgumentRegistersFromList(StubAssembler&, const ArgumentList&, unsigned firstRegister = 0);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
