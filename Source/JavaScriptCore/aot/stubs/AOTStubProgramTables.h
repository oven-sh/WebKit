/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTProgramTables.h"
#include "AOTStubHelpers.h"
#include <bit>

namespace JSC { namespace AOT {

constexpr ptrdiff_t offsetOfProgramTableRange(ProgramTableIndex index)
{
    return OBJECT_OFFSETOF(ProgramTables, ranges) + static_cast<unsigned>(index) * sizeof(TableRange);
}

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

inline void loadAddressOfClosureFacts(StubAssembler& jit, GPRReg function, GPRReg facts)
{
    static_assert(sizeof(ClosureFacts) == 4);
    ASSERT(noOverlap(function, facts));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.closureFactNumbers)), facts);
    jit.load16(BaseIndex(facts, function, CCallHelpers::TimesTwo), facts);
    jit.lshift32(TrustedImm32(2), facts);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.closureFacts)), facts);
}

inline void loadKindOfClosureFunction(StubAssembler& jit, GPRReg function, GPRReg kind)
{
    static_assert(sizeof(FunctionKindData) == 2);
    loadAddressOfClosureFacts(jit, function, kind);
    jit.load16(Address(kind, OBJECT_OFFSETOF(ClosureFacts, kind)), kind);
}

inline uint32_t slimFunctionKindBit()
{
    FunctionKindData kind { };
    kind.isSlim = 1;
    return std::bit_cast<uint16_t>(kind);
}

inline uint32_t asyncFunctionKindBit()
{
    FunctionKindData kind { };
    kind.isAsync = 1;
    return std::bit_cast<uint16_t>(kind);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
