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

struct MentionedAllocation {
    PlacedAllocation placed;
    uint32_t mention { 0 };
};

enum class ZeroedBy : uint8_t { Nobody, Stub, CodeInLine, StubFromTemplate };

class CellAllocationEmitter final : public OperationEmitter {
public:
    explicit CellAllocationEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    CellPointer allocateHeapCell(Pointer allocatorOrNull, LBasicBlock slowPath);
    CellPointer allocateHeapCell(Pointer allocator, std::optional<size_t> cellSize, LBasicBlock slowPath);
    void storeHeader(CellPointer, Integer header);

    CellPointer allocateObject(InlineAllocation, LBasicBlock slowPath);
    CellPointer allocateCellWithHeader(InlineAllocation, LBasicBlock slowPath);
    MentionedAllocation allocationOf(const OperationContext&, LayoutID, std::span<const FieldIndex> initialFields);
    MentionedAllocation environmentAllocationOf(LayoutID);
    MentionedAllocation closureAllocationOf(unsigned numberOfCaptures);
    Pointer allocationSiteRecord(const MentionedAllocation&);
    CellPointer allocate(const OperationContext&, const MentionedAllocation&, ZeroedBy = ZeroedBy::Nobody);
    std::pair<CellPointer, Condition> allocateInstance(const OperationContext&, const MentionedAllocation&, CellPointer prototype);
    ZeroedBy whoZeroesBody(const OperationContext&, const MentionedAllocation&, unsigned storesOfZero, unsigned storesOfOtherPatterns) const;
    CellPointer allocateFromTemplate(const OperationContext&, const MentionedAllocation&, std::span<const uint8_t> bodyOfEmptyCell);
    CellPointer allocateObjectCopy(const OperationContext&, const MentionedAllocation&, CellPointer source, unsigned fieldBytes);
    void copyFieldWords(CellPointer source, CellPointer cell, unsigned fieldBytes);
    void copyWords(CellPointer source, unsigned offsetInSource, CellPointer cell, unsigned offsetInCell, unsigned bytes);
    static constexpr size_t maxBytesZeroedInLine = 256;
    static constexpr unsigned bytesStoredInTurnOfLoop = 8 * sizeof(uint64_t);
    void storeWordInLoop(CellPointer, const Layout&, unsigned begin, unsigned end, int64_t word);
    CellPointer allocateInFrame(const OperationContext&, size_t size);

    std::pair<CellPointer, Pointer> allocateArray(const OperationContext&, Word32 length, Word32 vectorLength, IndexingType, LBasicBlock slowPath);
    CellPointer allocateArrayOfLength(const OperationContext&, IndexingType, unsigned length);
    void finishArrayOfLength(const OperationContext&, CellPointer array, unsigned length);
    CellPointer newArrayOf(const OperationContext&, std::span<const Boxed> elements);
    CellPointer newArrayFromBuffer(const OperationContext&, Word32 count, Pointer buffer);

private:
    Integer rowOf(const MentionedAllocation& allocation) { return zeroExtended(numberOf(allocation.mention)); }
    void zeroBody(CellPointer, const MentionedAllocation&);
    LValue slotOfFrameCell(const OperationContext&, size_t size);

    struct SlotOfClosuresLent {
        Machine::StackSlot* slot { nullptr };
        uint32_t blockIndex { 0 };
        ValueID call;
    };
    Vector<SlotOfClosuresLent, 4> m_slotsOfClosuresLent;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
