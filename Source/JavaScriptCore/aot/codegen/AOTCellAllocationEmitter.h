/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "IndexingType.h"
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

class CellAllocationEmitter final : public OperationEmitter {
public:
    explicit CellAllocationEmitter(CodeGenerator&);

    CellPointer allocateHeapCell(Pointer allocatorOrNull, LBasicBlock slowPath);
    // cellSize is the size of the allocator's cells, which is the size asked for only up to MarkedSpace::preciseCutoff.
    CellPointer allocateHeapCell(Pointer allocator, std::optional<size_t> cellSize, LBasicBlock slowPath);
    Pointer allocatorForSize(Pointer subspace, Integer size, LBasicBlock slowPath);
    // Null until the subspace has an allocator for the size step.
    Pointer allocatorForSize(Pointer subspace, size_t);
    void storeHeader(CellPointer, Integer header);
    void fillWords(Pointer base, Integer beginIndex, Integer endIndex, Integer word, const B3::AbstractHeap&);

    CellPointer allocateObject(InlineAllocation, LBasicBlock slowPath);
    Pointer allocationSiteRecord(AllocationID);
    CellPointer allocate(const OperationContext&, AllocationID);
    CellPointer allocateObjectCopy(const OperationContext&, AllocationID, CellPointer source, unsigned fieldBytes);
    CellPointer allocateInFrame(size_t size);

    // The elements below the length are the caller's to store before anything can collect.
    std::pair<CellPointer, Pointer> allocateArray(const OperationContext&, Word32 length, Word32 vectorLength, IndexingType, LBasicBlock slowPath);
    CellPointer newArrayOf(const OperationContext&, std::span<const Boxed> elements);
    CellPointer newArrayFromBuffer(const OperationContext&, Word32 count, Pointer buffer);
    CellPointer allocateRope(LBasicBlock slowPath);
    void storeRopeFibers(CellPointer rope, Integer fiber0, Integer length, Integer fiber1, Integer fiber2);
    CellPointer newPromise(LBasicBlock slowPath);

private:
    // The record lies at distances + distance - sizeof(AllocationSiteRecord). Kept apart, the two fold into the address of every access.
    struct AllocationSiteAddress {
        Pointer distances;
        Integer distance;
    };

    AllocationSiteAddress allocationSiteAddress(AllocationID);
    Pointer recordAt(const AllocationSiteAddress&);
    void copyHeaderAndWords(const AllocationSiteAddress&, CellPointer);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
