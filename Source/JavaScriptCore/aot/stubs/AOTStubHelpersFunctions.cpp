/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubHelpers.h"

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTInstance.h"
#include "AOTStubRegisters.h"
#include <algorithm>

namespace JSC { namespace AOT {

void toCodeAddress(StubAssembler& jit, GPRReg word, GPRReg scratch, StackIsTested stackIsTested)
{
    ASSERT(noOverlap(word, scratch, instanceGPR));
    if (stackIsTested == StackIsTested::No)
        jit.checkStackBeforeEntering();
    static_assert(ClosureWord::numberOfOffsetBits == 32);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.code)), scratch);
#if CPU(ARM64)
    static_assert(ClosureWord::offsetUnit == 4);
    jit.m_assembler.add<64>(word, scratch, word, ARM64Assembler::UXTW, 2);
#else
    static_assert(ClosureWord::offsetUnit == 4 || ClosureWord::offsetUnit == 1);
    jit.zeroExtend32ToWord(word, word);
    jit.getEffectiveAddress(BaseIndex(scratch, word, ClosureWord::offsetUnit == 4 ? CCallHelpers::TimesFour : CCallHelpers::TimesOne), word);
#endif
}

void checkStubOperands(StubKey target, std::span<const GPRReg> registers)
{
    const StubSignature signature = signatureOf(target);
    bool areOperandsOfTarget = std::ranges::equal(std::span(signature.operands).first(signature.numberOfOperands), registers, [](Reg operand, GPRReg reg) {
        return operand == Reg(reg);
    });
    RELEASE_ASSERT(areOperandsOfTarget);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
