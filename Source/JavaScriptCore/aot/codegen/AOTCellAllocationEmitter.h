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

// Who writes zero into every byte of a fresh cell behind its header, or behind its words.
enum class ZeroedBy : uint8_t { Nobody, Stub, CodeInLine };

class PlaceInCell;

class WholeCell {
private:
    friend class PlaceInCell;
    WholeCell() = default;
};

class CellAllocationEmitter final : public OperationEmitter {
public:
    explicit CellAllocationEmitter(CodeGenerator&);

    CellPointer allocateHeapCell(Pointer allocatorOrNull, LBasicBlock slowPath);
    // cellSize is the size of the allocator's cells, which is the size asked for only up to MarkedSpace::preciseCutoff.
    CellPointer allocateHeapCell(Pointer allocator, std::optional<size_t> cellSize, LBasicBlock slowPath);
    Pointer allocatorForSize(Pointer subspace, Integer size, LBasicBlock isTooLarge, LBasicBlock hasNoAllocator);
    // Null until the subspace has an allocator for the size step.
    Pointer allocatorForSize(Pointer subspace, size_t);
    void storeHeader(CellPointer, Integer header);

    CellPointer allocateObject(InlineAllocation, LBasicBlock slowPath);
    Pointer allocationSiteRecord(AllocationID);
    // By the stub, in code of every shape: in line it is 130 bytes a site, eleven by the stub, and no faster where the code around it has to be fetched.
    CellPointer allocate(const OperationContext&, AllocationID, WholeCell, ZeroedBy = ZeroedBy::Nobody);
    CellPointer allocateCellOfBothObjects(const OperationContext&, const PlaceInCell&);
    CellPointer allocateZeroedByNumber(const OperationContext&, Word32 numberOfAllocation, WholeCell);
    // Sixteen bytes of zero take one store. Somebody writes them where that takes no more instructions than the stores of zero that the caller then leaves out.
    ZeroedBy whoZeroesBody(const OperationContext&, AllocationID, unsigned storesOfZero) const;
    // For a cell that lies in the cell of another allocation, behind that allocation: whoever fills the record of that one has filled this one,
    // unless a class constructs the cell: then the code asks in front of the allocation.
    void copyHeaderAndWords(AllocationID, CellPointer);
    void ensureRecordIsFilled(const OperationContext&, AllocationID);
    CellPointer allocateObjectCopy(const OperationContext&, AllocationID, WholeCell, CellPointer source, unsigned fieldBytes);
    CellPointer allocateInFrame(size_t size);

    // The elements below the length are the caller's to store before anything can collect.
    std::pair<CellPointer, Pointer> allocateArray(const OperationContext&, Word32 length, Word32 vectorLength, IndexingType, LBasicBlock slowPath);
    CellPointer newArrayOf(const OperationContext&, std::span<const Boxed> elements);
    CellPointer newArrayFromBuffer(const OperationContext&, Word32 count, Pointer buffer);
    // The cell, the indexing header and the elements lie in the frame one after the other. On the heap where the realm has no pristine arrays, or the buffer holds more than the frame has room for.
    CellPointer newArrayInFrameOf(const OperationContext&, std::span<const Boxed> elements);
    CellPointer newArrayInFrameFromBuffer(const OperationContext&, Word32 count, Pointer buffer, unsigned capacity);
    CellPointer allocateRope(LBasicBlock slowPath);
    void storeRopeFibers(CellPointer rope, Integer fiber0, Integer length, Integer fiber1, Integer fiber2);

private:
    CellPointer allocateWithoutPlace(const OperationContext&, AllocationID, ZeroedBy);
    // The record lies at distances + distance - sizeof(AllocationSiteRecord). Kept apart, the two fold into the address of every access.
    struct AllocationSiteAddress {
        Pointer distances;
        Integer distance;
    };

    std::pair<CellPointer, Pointer> allocateArrayInFrame(Word32 length, unsigned capacity, LBasicBlock isOnHeap);
    AllocationSiteAddress allocationSiteAddress(AllocationID);
    Pointer recordAt(const AllocationSiteAddress&);
    void copyHeaderAndWords(const AllocationSiteAddress&, CellPointer);
    void zeroBody(CellPointer, AllocationID);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
