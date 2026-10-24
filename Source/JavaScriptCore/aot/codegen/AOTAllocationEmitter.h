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
    explicit AllocationEmitter(CodeGenerator&);

    PlaceInCell placeInCellOf(const ValueContext&);
    PlaceInCell placeInCellOf(const Value&) const;

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
    void generateNewSuppressedError(const ValueContext&);
    void generateCreateUnmappedArguments(const ValueContext&);
    void generateCreateMappedArguments(const ValueContext&);
    void generateGetMappedArgument(const ValueContext&);
    void generateSetMappedArgument(const ValueContext&);

    CellPointer allocateWithFields(const ValueContext&, AllocationID, std::span<const uint32_t> fields, std::span<const ValueID> values, AllocationStorage);
    // For a switch on an integer. Answers with the blocks that it has generated. The code that follows is reached with every value that leads to none of them.
    Vector<BlockID, 16> allocateByTable(const TerminatorContext&, LValue subject);

    // For an operation that makes a cell of another class, an array say, in the cell of the object that will list it.
    struct RoomForInnerCell {
        CellPointer outer;
        CellPointer inner;
        uint32_t arrayCapacity { 0 };
        LayoutID outerLayout;
        uint32_t field { 0 };
    };
    std::optional<RoomForInnerCell> allocateRoomForInnerCell(const ValueContext&);
    void didFillInnerCell(const RoomForInnerCell&);

private:
    enum class EmptySlots : uint8_t { All, HoldingCells, HoldingNoCells };

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
    enum class CellBody : uint8_t { Unwritten, Zero };
    CellPointer allocateCell(const ValueContext&, AllocationID, const PlannedLayout&, AllocationStorage, WholeCell, ZeroedBy);
    ZeroedBy whoZeroesBodyOfFreshCell(const ValueContext&, AllocationID, const BitVector& isInitialField, AllocationStorage);
    template<typename Visitor> void forEachEmptySlot(LayoutID, const BitVector& isInitialField, EmptySlots, const Visitor&);
    AllocationID allocationOfOuterObject(LayoutID outerLayout, const InnerObject&) const;
    CellPointer allocateBehindOuterObject(const ValueContext&, AllocationID, const PlaceInCell&);
    CellPointer cellAroundInnerObject(const ValueContext&, const PlaceInCell&);
    TypedPointer addressOfReferenceToInnerObject(CellPointer outer, LayoutID outerLayout, uint32_t field);
    void storeReferenceTo(CellPointer inner, CellPointer outer, LayoutID outerLayout, uint32_t field);
    CellPointer takeReleasedCellOrAllocate(const ValueContext&, AllocationID, WholeCell, uint32_t freeCellOffset);
    void initializeHiddenFields(CellPointer, LayoutID, AllocationStorage, CellBody = CellBody::Unwritten);
    void storeInitialFields(CellPointer, LayoutID, const InitialFields&);
    void storeEmptySlots(CellPointer, LayoutID, const BitVector& isInitialField, EmptySlots = EmptySlots::All, CellBody = CellBody::Unwritten);
    void emitBarrierIfReleased(const ValueContext&, CellPointer, LayoutID, const InitialFields&);
    void storeFieldsInExtension(const ValueContext&, CellPointer, LayoutID, const InitialFields&, OwnerBarrier);
    void storePrototype(const ValueContext&, CellPointer, ValueID prototype, std::optional<Word32> numberOfAllocation = std::nullopt);
    Vector<WordOfEmptyObject, 8> wordsOfEmptyObject(LayoutID);
    ValueID findOnlyAllocationIn(BlockID from, const Target&);
    std::optional<AllocationsByCase> findAllocationsByCase(const TerminatorContext&);
    void generateNewObjectChosenByTable(const ValueContext&, const AllocationChosenByTable&);
    struct CopyThatFills {
        ValueID copy;
        WholeCell cell;
    };
    std::optional<CopyThatFills> findCopyThatFills(const ValueContext&);
    CellPointer createArguments(const ValueContext&, RuntimeOperation, std::optional<CellPointer> callee);

    AllocationChosenByTable m_allocationChosenByTable;
    BitVector m_liesBehindOuterObject;
};

class PlaceInCell {
public:
    enum class Kind : uint8_t { OwnCell, BehindOuterObject, AroundInnerObject, Inconsistent };

    Kind kind() const { return m_kind; }
    uint32_t offsetOfInnerObject() const { return m_offsetOfInnerObject; }
    uint32_t arrayCapacity() const { return m_arrayCapacity; }
    uint32_t field() const { return m_field; }
    LayoutID outerLayout() const { return m_outerLayout; }
    AllocationID outerAllocation() const { return m_outerAllocation; }
    ValueID innerObject() const { return m_innerObject; }

    std::optional<WholeCell> ownCell() const { return m_kind == Kind::OwnCell ? std::optional<WholeCell>(WholeCell()) : std::nullopt; }

private:
    friend PlaceInCell AllocationEmitter::placeInCellOf(const Value&) const;
    PlaceInCell() = default;

    Kind m_kind { Kind::OwnCell };
    uint32_t m_offsetOfInnerObject { 0 };
    uint32_t m_arrayCapacity { 0 };
    uint32_t m_field { 0 };
    LayoutID m_outerLayout;
    AllocationID m_outerAllocation;
    ValueID m_innerObject;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
