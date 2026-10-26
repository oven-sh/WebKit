/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubHelpers.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTInstance.h"
#include "AOTStubRegisters.h"

namespace JSC { namespace AOT {

static void countCellOfStub(StubAssembler& jit, GPRReg allocator, GPRReg counts, GPRReg cellSize)
{
    if (!jit.options().countsAllocations)
        return;
    size_t kind = static_cast<size_t>(jit.key().family());
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.counters.allocationCounts)), counts);
    jit.load32(Address(allocator, LocalAllocator::offsetOfCellSize()), cellSize);
    jit.add64(TrustedImm32(1), Address(counts, static_cast<int32_t>(ExactAllocationCounts::offsetOfCells(kind))));
    jit.add64(cellSize, Address(counts, static_cast<int32_t>(ExactAllocationCounts::offsetOfBytes(kind))));
}

void countAllocatedCellOfStub(StubAssembler& jit, GPRReg scratch, unsigned cellSize)
{
    if (!jit.options().countsAllocations)
        return;
    size_t kind = static_cast<size_t>(jit.key().family());
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.counters.allocationCounts)), scratch);
    jit.add64(TrustedImm32(1), Address(scratch, static_cast<int32_t>(ExactAllocationCounts::offsetOfCells(kind))));
    jit.add64(TrustedImm32(static_cast<int32_t>(cellSize)), Address(scratch, static_cast<int32_t>(ExactAllocationCounts::offsetOfBytes(kind))));
}

void countCellOfAllocation(StubAssembler& jit, GPRReg allocation, GPRReg scratch)
{
    if (!jit.options().countsAllocations)
        return;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.counters.allocationCounts)), scratch);
    jit.add64(TrustedImm32(1), BaseIndex(scratch, allocation, CCallHelpers::TimesEight, static_cast<int32_t>(ExactAllocationCounts::offsetOfCellsOfAllocations())));
}

void allocateCell(StubAssembler& jit, GPRReg result, GPRReg allocator, GPRReg scratch, JumpList& slowPath)
{
    ASSERT(noOverlap(result, allocator, scratch));
    countCellOfStub(jit, allocator, result, scratch);
    if (jit.options().forceGCSlowPaths) {
        slowPath.append(jit.jump());
        return;
    }
    AssemblyHelpers::FreeListRefill refill = jit.emitAllocateFromFreeList(result, allocator, scratch, slowPath);
    Jump done = jit.jump();
    jit.emitFreeListRefill(refill, slowPath);
    done.link(&jit);
}

AssemblyHelpers::FreeListRefill allocateCellWithLateRefill(StubAssembler& jit, GPRReg result, GPRReg allocator, GPRReg scratch, JumpList& slowPath, std::optional<unsigned> cellSize, CountedUnder countedUnder)
{
    ASSERT(noOverlap(result, allocator, scratch));
    if (countedUnder == CountedUnder::FamilyOfStub)
        countCellOfStub(jit, allocator, result, scratch);
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
