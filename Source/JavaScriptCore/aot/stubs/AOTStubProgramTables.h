/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
