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

// A cell as Plan has placed it, and the mention of its row in the image's table of allocations (NumberSpace::Allocation).
struct MentionedAllocation {
    PlacedAllocation placed;
    uint32_t mention { 0 };
};

// Who writes zero into every byte of a fresh cell behind its header, or behind its words.
// StubFromTemplate: the stub copies what an empty cell of the layout holds, so a slot that is not zero while it is empty has its pattern as well.
enum class ZeroedBy : uint8_t { Nobody, Stub, CodeInLine, StubFromTemplate };

class CellAllocationEmitter final : public OperationEmitter {
public:
    explicit CellAllocationEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    CellPointer allocateHeapCell(Pointer allocatorOrNull, LBasicBlock slowPath);
    // cellSize is the size of the allocator's cells, which is the size asked for only up to MarkedSpace::preciseCutoff.
    CellPointer allocateHeapCell(Pointer allocator, std::optional<size_t> cellSize, LBasicBlock slowPath);
    void storeHeader(CellPointer, Integer header);

    CellPointer allocateObject(InlineAllocation, LBasicBlock slowPath);
    // The header is stored, and nothing behind it.
    CellPointer allocateCellWithHeader(InlineAllocation, LBasicBlock slowPath);
    // Plan::allocation(). Fails the build where the image has no key order that begins so: the walk asks Plan the same question before it builds the operation.
    MentionedAllocation allocationOf(const OperationContext&, LayoutID, AllocationClassID, std::span<const FieldIndex> initialFields);
    MentionedAllocation environmentAllocationOf(LayoutID);
    MentionedAllocation closureAllocationOf(unsigned numberOfCaptures);
    Pointer allocationSiteRecord(const MentionedAllocation&);
    CellPointer allocate(const OperationContext&, const MentionedAllocation&, ZeroedBy = ZeroedBy::Nobody);
    // The object that the prototype selects. Only where the condition holds is it the cell of the allocation, zeroed behind its words, and the caller's to fill.
    std::pair<CellPointer, Condition> allocateInstance(const OperationContext&, const MentionedAllocation&, CellPointer prototype);
    // Sixteen bytes of zero take one store. Somebody writes them where that takes no more instructions than the stores of zero that the caller then leaves out.
    // A copy of sixteen bytes takes a load and a store. The stub copies the template where that, too, takes no more instructions than all that the caller then leaves out.
    ZeroedBy whoZeroesBody(const OperationContext&, const MentionedAllocation&, unsigned storesOfZero, unsigned storesOfOtherPatterns) const;
    // For ZeroedBy::StubFromTemplate. The template is sizeOfCellTemplate() long. It is a constant of the function: Image keeps equal ones once.
    CellPointer allocateFromTemplate(const OperationContext&, const MentionedAllocation&, std::span<const uint8_t> bodyOfEmptyCell);
    CellPointer allocateObjectCopy(const OperationContext&, const MentionedAllocation&, CellPointer source, unsigned fieldBytes);
    // The words behind the header of one cell into another of the same size, in chunks: a few one behind the other, more in a loop.
    void copyFieldWords(CellPointer source, CellPointer cell, unsigned fieldBytes);
    CellPointer allocateInFrame(const OperationContext&, size_t size);

    // The elements below the length are the caller's to store before anything can collect.
    std::pair<CellPointer, Pointer> allocateArray(const OperationContext&, Word32 length, Word32 vectorLength, IndexingType, LBasicBlock slowPath);
    // An array of 1 to maxArrayLengthInStubKey elements in plain storage. The caller stores every element at once, and then calls finishArrayOfLength().
    CellPointer allocateArrayOfLength(const OperationContext&, IndexingType, unsigned length);
    void finishArrayOfLength(const OperationContext&, CellPointer array, unsigned length);
    CellPointer newArrayOf(const OperationContext&, std::span<const Boxed> elements);
    CellPointer newArrayFromBuffer(const OperationContext&, Word32 count, Pointer buffer);
    // For whoever only reads the array and lets go of it before the frame ends. It lies in the frame where it has no more elements than the capacity and the realm makes plain arrays, and in the heap elsewhere.
    CellPointer newArrayInFrameFromBuffer(const OperationContext&, Word32 count, Pointer buffer, unsigned capacity);
    // The same for a literal of values.
    CellPointer newArrayInFrameOf(const OperationContext&, std::span<const Boxed> elements);

private:
    // The elements below the length are the caller's to store.
    std::pair<CellPointer, Pointer> allocateArrayInFrame(const OperationContext&, Word32 length, unsigned capacity, LBasicBlock isOnHeap);
    // As the stubs take it: the upper 32 bits are zero.
    Integer rowOf(const MentionedAllocation& allocation) { return zeroExtended(numberOf(allocation.mention)); }
    void copyFromBuffer(Word32 count, Pointer buffer, Pointer butterfly, LBasicBlock copyLoop, LBasicBlock copyElement, LBasicBlock copied);
    void zeroBody(CellPointer, const MentionedAllocation&);
    // The address of the words of the cell that the operation makes. Cells that are lent under one number (CellStamp::numberAmongLoans) are never alive at one time, so those of one size have the same words.
    LValue slotOfFrameCell(const OperationContext&, size_t size);

    struct SlotOfLoans {
        uint32_t numberAmongLoans { 0 };
        size_t size { 0 };
        Machine::StackSlot* slot { nullptr };
    };
    Vector<SlotOfLoans, 4> m_slotsOfLoans;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
