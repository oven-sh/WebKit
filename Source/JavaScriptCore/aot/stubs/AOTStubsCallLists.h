/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsCallFrames.h"
#include "AOTStubsCalls.h"
#include "AOTThreshold.h"

namespace JSC { namespace AOT {

constexpr StubKey callWithArray() { return StubKey(StubFamily::CallWithArray); }
constexpr StubKey callWithList() { return StubKey(StubFamily::CallWithList); }
constexpr StubKey tailCallWithList() { return StubKey(StubFamily::TailCallWithList); }
constexpr StubKey superConstructWithList() { return StubKey(StubFamily::SuperConstructWithList); }
constexpr StubKey callFromArgumentListFrame() { return StubKey(StubFamily::CallFromArgumentListFrame); }
constexpr StubKey returnFromArgumentListFrame() { return StubKey(StubFamily::ReturnFromArgumentListFrame); }

AOT_THRESHOLD(unsigned, maxArrayLengthCopiedToStack, 4096);

struct ArgumentList {
    GPRReg count;
    GPRReg words;
};

void emitLoadArgumentRegistersFromList(StubAssembler&, const ArgumentList&, unsigned firstRegister = 0);
// The bytes that count + extraWords words take on the stack.
void emitAlignedSizeOfWords(StubAssembler&, GPRReg count, int32_t extraWords, GPRReg size);
// index is the number of words, which is not zero.
void emitCopyFromLastWord(StubAssembler&, Address source, Address destination, GPRReg index, GPRReg word);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
