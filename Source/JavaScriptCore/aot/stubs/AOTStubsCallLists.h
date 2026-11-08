/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubsCallFrames.h"
#include "AOTStubsCalls.h"

namespace JSC { namespace AOT {

constexpr StubKey callWithArray(CallPosition position = CallPosition::Ordinary) { return StubKey(StubFamily::CallWithArray).withForm(static_cast<uint8_t>(position)); }
constexpr StubKey tailCallWithList() { return StubKey(StubFamily::TailCallWithList); }
constexpr StubKey superConstructWithArray() { return StubKey(StubFamily::SuperConstructWithArray); }
constexpr StubKey callFromArgumentListFrame() { return StubKey(StubFamily::CallFromArgumentListFrame); }
constexpr StubKey returnFromArgumentListFrame() { return StubKey(StubFamily::ReturnFromArgumentListFrame); }
constexpr StubKey callFromArgumentListFrameWithWords(unsigned numberOfWords, unsigned bytesAboveFrameRecord)
{
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(numberOfWords && numberOfWords <= StubKey::maxCount && (!bytesAboveFrameRecord || bytesAboveFrameRecord == sizeOfSpilledArgumentRegisters));
    return StubKey(StubFamily::CallFromArgumentListFrameWithWords).withForm(!!bytesAboveFrameRecord).withCount(numberOfWords);
}

static constexpr unsigned maxListLengthCopiedTwice = 4096;

struct ArgumentList {
    GPRReg count;
    GPRReg words;
};

void emitLoadArgumentRegistersFromList(StubAssembler&, const ArgumentList&, unsigned firstRegister = 0);
void emitAlignedSizeOfWords(StubAssembler&, GPRReg count, int32_t extraWords, GPRReg size);
void emitCopyFromLastWord(StubAssembler&, Address source, Address destination, GPRReg index, GPRReg word);
void emitCopyFromFirstWord(StubAssembler&, GPRReg source, GPRReg destination, GPRReg count, GPRReg index, GPRReg word);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
