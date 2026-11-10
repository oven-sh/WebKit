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
    void generateNewSuppressedError(const ValueContext&);
    void generateCreateUnmappedArguments(const ValueContext&);
    void generateCreateMappedArguments(const ValueContext&);
    void generateGetMappedArgument(const ValueContext&);
    void generateSetMappedArgument(const ValueContext&);

    CellPointer allocateWithFields(const ValueContext&, const MentionedAllocation&, std::span<const uint32_t> fields, std::span<const ValueID> values, AllocationStorage);

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
    };

    void clearBirthMark(CellPointer, const Type&);
    InitialFields collectInitialFields(LayoutID, std::span<const uint32_t> fields, std::span<const ValueID> values);
    CellPointer allocateCell(const ValueContext&, const MentionedAllocation&, const PlannedLayout&, AllocationStorage, ZeroedBy);
    CellPointer takeCellHandedBackOrAllocate(const ValueContext&, const MentionedAllocation&);
    bool initializesFrameObjectInStub(const ValueContext&, LayoutID);
    CellPointer allocateFrameCell(const ValueContext&, const MentionedAllocation&, const PlannedLayout&);
    using EmptySlots = Vector<FieldIndex, 8>;
    EmptySlots collectEmptySlots(LayoutID, const BitVector& isInitialField);
    ZeroedBy whoZeroesBodyOfFreshCell(const ValueContext&, const MentionedAllocation&, std::span<const FieldIndex> emptySlots, AllocationStorage);
    template<typename Visitor> void forEachEmptySlot(LayoutID, const BitVector& isInitialField, const Visitor&);
    void initializeHiddenFields(CellPointer, LayoutID, AllocationStorage, ZeroedBy, bool hasStateOfFrameCell);
    static Representation representationWantedBy(const InitialField&);
    bool isNarrowedWhereStored(const InitialField&) const;
    Operand takeForSlot(const InitialField&);
    void storeInitialFields(CellPointer, LayoutID, const InitialFields&);
    void storeEmptySlots(CellPointer, LayoutID, std::span<const FieldIndex> emptySlots, ZeroedBy, BlockID);
    void storeFieldsInExtension(const ValueContext&, CellPointer, LayoutID, const InitialFields&);
    void storePrototype(const ValueContext&, CellPointer, ValueID prototype);
    bool looksAtPrototypeBeforeAllocating(const ValueContext&);
    CellPointer allocateInstance(const ValueContext&, const MentionedAllocation&, ValueID prototype);
    CellPointer allocateCopyFromOtherLayout(const ValueContext&, ValueID copy);
    Vector<WordOfEmptyObject, 8> wordsOfEmptyObject(LayoutID);
    Vector<uint8_t, 64> bodyOfEmptyCell(const MentionedAllocation&);
    CellPointer createArguments(const ValueContext&, std::optional<CellPointer> callee);
    bool isListedByArgumentsObjectAlone(ValueID list) const;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
