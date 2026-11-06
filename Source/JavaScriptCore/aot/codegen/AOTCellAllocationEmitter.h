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

struct PlannedLayout;

// Who writes zero into every byte of a fresh cell behind its header, or behind its words.
enum class ZeroedBy : uint8_t { Nobody, Stub, CodeInLine };

class CellAllocationEmitter final : public OperationEmitter {
public:
    explicit CellAllocationEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    CellPointer allocateHeapCell(Pointer allocatorOrNull, LBasicBlock slowPath);
    // cellSize is the size of the allocator's cells, which is the size asked for only up to MarkedSpace::preciseCutoff.
    CellPointer allocateHeapCell(Pointer allocator, std::optional<size_t> cellSize, LBasicBlock slowPath);
    Pointer allocatorForSize(Pointer subspace, Integer size, LBasicBlock isTooLarge, LBasicBlock hasNoAllocator);
    // Null until the subspace has an allocator for the size step.
    Pointer allocatorForSize(Pointer subspace, size_t);
    void storeHeader(CellPointer, Integer header);

    CellPointer allocateObject(InlineAllocation, LBasicBlock slowPath);
    // The header is stored, and nothing behind it.
    CellPointer allocateCellWithHeader(InlineAllocation, LBasicBlock slowPath);
    Pointer allocationSiteRecord(AllocationID);
    // By the stub, in code of every shape: in line it is 130 bytes a site, eleven by the stub, and no faster where the code around it has to be fetched.
    CellPointer allocate(const OperationContext&, AllocationID, ZeroedBy = ZeroedBy::Nobody);
    CellPointer allocateZeroedByNumber(const OperationContext&, Word32 numberOfAllocation);
    // The object that the prototype selects. Only where the condition holds is it the cell of the allocation, zeroed behind its words, and the caller's to fill.
    std::pair<CellPointer, Condition> allocateInstance(const OperationContext&, AllocationID, CellPointer prototype);
    // Sixteen bytes of zero take one store. Somebody writes them where that takes no more instructions than the stores of zero that the caller then leaves out.
    ZeroedBy whoZeroesBody(const OperationContext&, AllocationID, unsigned storesOfZero) const;
    CellPointer allocateObjectCopy(const OperationContext&, AllocationID, CellPointer source, unsigned fieldBytes);
    CellPointer allocateInFrame(const OperationContext&, size_t size, const PlannedLayout* = nullptr);
    // In a build that validates or checks proofs at run time, emitted before anything that leaves or saves the frame: the cells that the function kept in the frame end their lifetime here.
    void leaveFrameOfCheckedCells();

    // The elements below the length are the caller's to store before anything can collect.
    std::pair<CellPointer, Pointer> allocateArray(const OperationContext&, Word32 length, Word32 vectorLength, IndexingType, LBasicBlock slowPath);
    // An array of 1 to maxArrayLengthInStubKey elements in plain storage. The caller stores every element at once, and then calls finishArrayOfLength().
    CellPointer allocateArrayOfLength(const OperationContext&, IndexingType, unsigned length);
    void finishArrayOfLength(const OperationContext&, CellPointer array, unsigned length);
    // In a frame the cell, the indexing header and the elements lie one after the other. On the heap where the realm has no pristine arrays, or the buffer holds more than the frame has room for.
    CellPointer newArrayOf(const OperationContext&, std::span<const Boxed> elements, AllocationStorage = AllocationStorage::Heap);
    CellPointer newArrayFromBuffer(const OperationContext&, Word32 count, Pointer buffer);
    CellPointer newArrayInFrameFromBuffer(const OperationContext&, Word32 count, Pointer buffer, unsigned capacity);

private:
    std::pair<CellPointer, Pointer> allocateArrayInFrame(const OperationContext&, Word32 length, unsigned capacity, LBasicBlock isOnHeap);
    bool keepsCellsInFrame();
    Pointer wordOfFrame();
    void copyFromBuffer(Word32 count, Pointer buffer, Pointer butterfly, LBasicBlock copyLoop, LBasicBlock copyElement, LBasicBlock copied);
    void zeroBody(CellPointer, AllocationID);

    std::optional<bool> m_keepsCellsInFrame;
    B3::Air::StackSlot* m_wordOfFrame { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
