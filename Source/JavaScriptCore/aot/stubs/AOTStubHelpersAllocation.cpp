/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubHelpers.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubRegisters.h"

namespace JSC { namespace AOT {

void allocateCell(StubAssembler& jit, GPRReg result, GPRReg allocator, GPRReg scratch, JumpList& slowPath)
{
    ASSERT(noOverlap(result, allocator, scratch));
    if (jit.options().forceGCSlowPaths) {
        slowPath.append(jit.jump());
        return;
    }
    AssemblyHelpers::FreeListRefill refill = jit.emitAllocateFromFreeList(result, allocator, scratch, slowPath);
    Jump done = jit.jump();
    jit.emitFreeListRefill(refill, slowPath);
    done.link(&jit);
}

AssemblyHelpers::FreeListRefill allocateCellWithLateRefill(StubAssembler& jit, GPRReg result, GPRReg allocator, GPRReg scratch, JumpList& slowPath, std::optional<unsigned> cellSize)
{
    ASSERT(noOverlap(result, allocator, scratch));
    if (jit.options().forceGCSlowPaths) {
        slowPath.append(jit.jump());
        return { };
    }
    return jit.emitAllocateFromFreeList(result, allocator, scratch, slowPath, cellSize);
}

void emitLateRefill(StubAssembler& jit, const AssemblyHelpers::FreeListRefill& refill, JumpList& slowPath)
{
    jit.emitFreeListRefill(refill, slowPath);
}

void fenceAfterAllocation(StubAssembler& jit)
{
#if CPU(X86_64)
    UNUSED_PARAM(jit);
#else
    Jump isNotFenced = jit.branchTest8(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced)));
    jit.storeFence();
    isNotFenced.link(&jit);
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
