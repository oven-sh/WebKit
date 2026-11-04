/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubBodyConvention.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT {

static_assert([] {
    for (unsigned index = 0; index < StubBodyConvention::maxNumberOfOperandGPRs; ++index) {
        if (!noOverlap(StubBodyConvention::operandGPR(index), instanceGPR, cellCageBaseGPR))
            return false;
    }
    return true;
}());
static_assert(StubBodyConvention::resultGPR(0) != StubBodyConvention::resultGPR(1));

RegisterSet StubBodyConvention::registersClobberedByCalls()
{
    RegisterSet clobbered;
    for (unsigned index = 0; index < maxNumberOfOperandGPRs; ++index)
        clobbered.add(operandGPR(index), IgnoreVectors);
    for (unsigned index = 0; index < maxNumberOfOperandFPRs; ++index)
        clobbered.add(operandFPR(index), IgnoreVectors);
    for (unsigned index = 0; index < maxNumberOfResultGPRs; ++index)
        clobbered.add(resultGPR(index), IgnoreVectors);
    clobbered.add(resultFPR(), IgnoreVectors);
    clobbered.merge(unsavedCalleeSaves());
    clobbered.merge(RegisterSet::macroClobberedGPRs());
    clobbered.merge(RegisterSet::macroClobberedFPRs());
    return clobbered;
}

RegisterSet StubBodyConvention::registersSavedByCallee()
{
    RegisterSet saved = RegisterSet::allScalarRegisters();
    saved.exclude(registersClobberedByCalls());
    saved.exclude(RegisterSet::stackRegisters());
    saved.exclude(RegisterSet::reservedHardwareRegisters());
    saved.remove(instanceGPR);
    saved.remove(cellCageBaseGPR);
#if CPU(ARM64)
    saved.exclude(RegisterSet::runtimeTagRegisters());
#endif
    return saved;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
