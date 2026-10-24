/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsStrings.h"

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTThreshold.h"

namespace JSC { namespace AOT {

AOT_THRESHOLD(int32_t, maxLengthSearchedByBytes, 32);

StubSignature signatureOfFindCharacter8(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
}

// The index of the first unit from position on that equals codeUnit, or -1. Only bits 0 to 31 of length, codeUnit and position count.
void generateFindCharacter8(StubAssembler& jit)
{
    static_assert(leafArgumentGPR(0) == gprOf(StubRegister::A0) && leafArgumentGPR(1) == gprOf(StubRegister::A1));
    static_assert(leafArgumentGPR(2) == gprOf(StubRegister::A2) && leafArgumentGPR(3) == gprOf(StubRegister::A3));
    const GPRReg characters = jit.operand(0);
    const GPRReg length = jit.operand(1);
    const GPRReg codeUnit = jit.operand(2);
    const GPRReg position = jit.operand(3);
    const GPRReg result = jit.result();
    // On ARM64 the result shares its register with characters, and the leaf clobbers A4 and A5 anyway.
    const GPRReg index = result == characters ? jit.allocateScratch(StubRegister::A5) : result;
    const GPRReg unit = jit.allocateScratch(StubRegister::A4);
    RELEASE_ASSERT(result == GPRInfo::returnValueGPR);
    RELEASE_ASSERT(noOverlap(characters, length, codeUnit, position, index, unit));

    Jump isPastEnd = jit.branch32(CCallHelpers::AboveOrEqual, position, length);
    Jump needsLeaf = jit.branch32(CCallHelpers::Above, length, TrustedImm32(maxLengthSearchedByBytes));
    jit.zeroExtend32ToWord(position, index);
    Label next = jit.label();
    jit.load8(BaseIndex(characters, index, CCallHelpers::TimesOne), unit);
    Jump isFound = jit.branch32(CCallHelpers::Equal, unit, codeUnit);
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, length).linkTo(next, &jit);

    isPastEnd.link(&jit);
    jit.move(TrustedImm64(-1), result);
    jit.returnToCaller();

    isFound.link(&jit);
    jit.move(index, result);
    jit.returnToCaller();

    needsLeaf.link(&jit);
    jit.callLeaf(Entry::operationAOTStringFindCharacter8);
    jit.returnToCaller();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
