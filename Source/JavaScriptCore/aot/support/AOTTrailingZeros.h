/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CCallHelpers.h"

namespace JSC { namespace AOT {

ALWAYS_INLINE void countTrailingZerosOfNonZero(CCallHelpers& jit, GPRReg bits, GPRReg count)
{
#if CPU(X86_64)
    jit.m_assembler.tzcntq_rr(bits, count);
#else
    jit.countTrailingZeros64(bits, count);
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
