/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct PlacedAllocation;
struct PlannedLayout;
struct PlacedSlot;

class AllocationEmitter final : public OperationEmitter {
public:
    explicit AllocationEmitter(CodeGenerator&);

    void generateNewObject(const ValueContext&);
    void generateFinishObject(const ValueContext&);
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

private:
    struct InitialField {
        const PlacedSlot* slot { nullptr };
        uint32_t field { 0 };
        ValueID value;
    };
    using InitialFields = Vector<InitialField, 8>;

    InitialFields collectInitialFields(LayoutID, std::span<const uint32_t> fields, std::span<const ValueID> values);
    CellPointer allocateCell(const ValueContext&, AllocationID, const PlannedLayout&, AllocationStorage);
    CellPointer takeReleasedCellOrAllocate(const ValueContext&, AllocationID, uint32_t freeCellOffset);
    void initializeHiddenFields(CellPointer, LayoutID, AllocationStorage);
    void storeInitialFields(const ValueContext&, CellPointer, LayoutID, const InitialFields&);
    void storeEmptySlots(CellPointer, LayoutID, const BitVector& isInitialField);
    void emitBarrierIfReleased(const ValueContext&, CellPointer, LayoutID, const InitialFields&);
    void storeFieldsInExtension(const ValueContext&, CellPointer, LayoutID, const InitialFields&);
    void storePrototype(const ValueContext&, CellPointer, ValueID prototype);
    CellPointer createArguments(const ValueContext&, RuntimeOperation, std::optional<CellPointer> callee);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
