/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubRegisters.h"

namespace JSC { namespace AOT {

// The convention of a function with FunctionRole::StubBody: operands and results as below, every other register is saved by the callee.
struct StubBodyConvention {
    static constexpr unsigned maxNumberOfOperandGPRs = 4;
    static constexpr unsigned maxNumberOfOperandFPRs = 2;
    static constexpr unsigned maxNumberOfResultGPRs = 2;

    static constexpr GPRReg operandGPR(unsigned index)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(index < maxNumberOfOperandGPRs);
        return GPRInfo::toArgumentRegister(index);
    }

    static constexpr FPRReg operandFPR(unsigned index)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(index < maxNumberOfOperandFPRs);
        return FPRInfo::toArgumentRegister(index);
    }

    static constexpr GPRReg resultGPR(unsigned index)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(index < maxNumberOfResultGPRs);
        return index ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR;
    }

    static constexpr FPRReg resultFPR() { return FPRInfo::returnValueFPR; }

    static RegisterSet registersClobberedByCalls();
    static RegisterSet registersSavedByCallee();
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
