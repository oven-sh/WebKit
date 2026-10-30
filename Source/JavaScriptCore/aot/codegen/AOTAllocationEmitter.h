/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTOperationEmitter.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

struct PlacedAllocation;
struct PlannedLayout;
struct PlacedSlot;

class AllocationEmitter final : public OperationEmitter {
public:
    explicit AllocationEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateNewObject(const ValueContext&);
    void generateNewDictionary(const ValueContext&);
    void generateFinishObject(const ValueContext&);
    void generateFinishBirth(const ValueContext&);
    void generateTryFinishBirth(const ValueContext&);
    void generateNewArray(const ValueContext&);
    void generateAppendElement(const ValueContext&);
    void generateAppendHole(const ValueContext&);
    void generateNewRegExp(const ValueContext&);
    void generateNewTemplateObject(const ValueContext&);
    void generateNewRuntimeError(const ValueContext&);
    void generateNewFunctionThatThrows(const ValueContext&);
    void generateNewSuppressedError(const ValueContext&);
    void generateCreateUnmappedArguments(const ValueContext&);
    void generateCreateMappedArguments(const ValueContext&);
    void generateGetMappedArgument(const ValueContext&);
    void generateSetMappedArgument(const ValueContext&);

    CellPointer allocateWithFields(const ValueContext&, AllocationID, std::span<const uint32_t> fields, std::span<const ValueID> values, AllocationStorage);
    // For a switch on an integer. Answers with the blocks that it has generated. The code that follows is reached with every value that leads to none of them.
    Vector<BlockID, 16> allocateByTable(const TerminatorContext&, LValue subject);

private:
    struct InitialField {
        const PlacedSlot* slot { nullptr };
        uint32_t field { 0 };
        ValueID value;
    };
    using InitialFields = Vector<InitialField, 8>;

    struct WordOfEmptyObject {
        uint32_t offset { 0 };
        SlotRepresentation representation { SlotRepresentation::JSValue };

        friend bool operator==(const WordOfEmptyObject&, const WordOfEmptyObject&) = default;
    };
    // What all the layouts hold in one place is stored by one instruction. A layout may hold a few more words of one kind, in places of its own, which its rows list.
    struct WordsOfEmptyObjects {
        Vector<WordOfEmptyObject, 8> shared;
        SlotRepresentation representationOfOwnWords { SlotRepresentation::JSValue };
        unsigned maxOwnWords { 0 };
    };
    // A row: the number of the allocation in the low half, then a byte for each own word: its place, counted in words of that width.
    struct AllocationsByCase {
        int64_t first { 0 };
        Vector<uint32_t> rows;
        Vector<BlockID, 16> blocks;
        WordsOfEmptyObjects words;
    };
    struct AllocationChosenByTable {
        ValueID operation;
        LValue row { nullptr };
        WordsOfEmptyObjects words;
    };

    InitialFields collectInitialFields(LayoutID, std::span<const uint32_t> fields, std::span<const ValueID> values);
    CellPointer allocateCell(const ValueContext&, AllocationID, const PlannedLayout&, AllocationStorage, ZeroedBy);
    bool initializesFrameObjectInStub(const ValueContext&, LayoutID);
    CellPointer allocateFrameCell(const ValueContext&, AllocationID, const PlannedLayout&);
    ZeroedBy whoZeroesBodyOfFreshCell(const ValueContext&, AllocationID, const BitVector& isInitialField, AllocationStorage);
    template<typename Visitor> void forEachEmptySlot(LayoutID, const BitVector& isInitialField, const Visitor&);
    CellPointer takeReleasedCellOrAllocate(const ValueContext&, AllocationID, uint32_t freeCellOffset);
    void initializeHiddenFields(CellPointer, LayoutID, AllocationStorage, ZeroedBy, bool hasStateOfFrameCell);
    static Representation representationWantedBy(const InitialField&);
    bool isNarrowedWhereStored(const InitialField&) const;
    Operand takeForSlot(const InitialField&);
    void storeInitialFields(CellPointer, LayoutID, const InitialFields&);
    void storeEmptySlots(CellPointer, LayoutID, const BitVector& isInitialField, ZeroedBy);
    void barrierIfCellWasStored(const ValueContext&, CellPointer, const InitialFields&);
    void storeFieldsInExtension(const ValueContext&, CellPointer, LayoutID, const InitialFields&, OwnerBarrier);
    void storePrototype(const ValueContext&, CellPointer, ValueID prototype, std::optional<Word32> numberOfAllocation = std::nullopt);
    bool looksAtPrototypeBeforeAllocating(const ValueContext&);
    CellPointer allocateInstance(const ValueContext&, AllocationID, ValueID prototype);
    Vector<WordOfEmptyObject, 8> wordsOfEmptyObject(LayoutID);
    ValueID findOnlyAllocationIn(BlockID from, const Target&);
    std::optional<AllocationsByCase> findAllocationsByCase(const TerminatorContext&);
    void generateNewObjectChosenByTable(const ValueContext&, const AllocationChosenByTable&);
    ValueID findCopyThatFills(const ValueContext&);
    CellPointer createArguments(const ValueContext&, std::optional<CellPointer> callee);

    AllocationChosenByTable m_allocationChosenByTable;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
