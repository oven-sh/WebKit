/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTProgramTables.h"
#include "AOTStubHelpers.h"

namespace JSC { namespace AOT {

constexpr ptrdiff_t offsetOfProgramTableRange(ProgramTableIndex index)
{
    return OBJECT_OFFSETOF(ProgramTables, ranges) + static_cast<unsigned>(index) * sizeof(TableRange);
}

// In both functions, tables holds Instance::image.tables.
inline Address programTableCount(GPRReg tables, ProgramTableIndex index)
{
    return Address(tables, offsetOfProgramTableRange(index) + OBJECT_OFFSETOF(TableRange, count));
}

inline void loadProgramTable(StubAssembler& jit, GPRReg tables, ProgramTableIndex index, GPRReg result)
{
    ASSERT(noOverlap(tables, result));
    jit.load32(Address(tables, offsetOfProgramTableRange(index) + OBJECT_OFFSETOF(TableRange, offset)), result);
    jit.addPtr(tables, result);
}

// The function is one that closures are made of: it comes from a closure, or from the field of a closure that is yet to be made.
inline void loadKindOfClosureFunction(StubAssembler& jit, GPRReg function, GPRReg kind)
{
    static_assert(sizeof(FunctionKindData) == 2);
    ASSERT(noOverlap(function, kind));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.closureFunctionKindsOrigin)), kind);
    jit.load16(BaseIndex(kind, function, CCallHelpers::TimesTwo), kind);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
