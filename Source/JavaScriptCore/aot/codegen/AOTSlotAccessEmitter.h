/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct FieldAccess;

// Whether the keys of an object are at a place in an order of the image where the created fields come next, and the order word after them.
struct KeyOrderStep {
    Condition applies;
    Word32 orderWordAfter;
};

// Placement lists the places with the most likely one of each layout first.
enum class KeyOrderPlaces : uint8_t {
    MostLikely,
    Others,
    All,
};

class SlotAccessEmitter final : public OperationEmitter {
public:
    explicit SlotAccessEmitter(CodeGenerator&);

    void generateGetSlot(const ValueContext&);
    void generateGetSlotOrUndefined(const ValueContext&);
    void generateGetDeferredClosureSlot(const ValueContext&);
    void generatePutSlot(const ValueContext&);
    void generatePutSlotOrAdd(const ValueContext&);

    void didCreateField(const ValueContext&, CellPointer, const FieldAccess&);

    // An object that a constructor is still filling is expected to get the field from the store.
    bool isExpectedToCreate(AccessSiteID) const;
    bool mayCreateListedField(const FieldAccess&) const;
    std::optional<KeyOrderStep> testKeyOrderStep(CellPointer, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys, KeyOrderPlaces);
    void takeKeyOrderStepOr(CellPointer, const std::optional<KeyOrderStep>&, NOESCAPE const Invocable<void()> auto& otherwise);

private:
    const FieldAccess& fieldAccessOf(const ValueContext&) const;
    Operand readSlot(const ValueContext&, CellPointer, const FieldAccess&, bool absentIsUndefined);
    void writeSlot(const ValueContext&, CellPointer, const FieldAccess&, ValueID stored);

    Operand loadSlot(CellPointer, const FieldAccess&);
    Operand loadFilledSlot(const ValueContext&, CellPointer, const FieldAccess&);
    Operand loadSlotOrUndefined(const ValueContext&, CellPointer, const FieldAccess&);
    Operand loadSlotOfMembersWithField(const ValueContext&, CellPointer, const FieldAccess&);
    Operand inResultRepresentation(const ValueContext&, Operand loaded);
    Operand undefinedResult(const ValueContext&);
    Operand failRead(const ValueContext&, InternalErrorKind);
};

void SlotAccessEmitter::takeKeyOrderStepOr(CellPointer cell, const std::optional<KeyOrderStep>& step, NOESCAPE const Invocable<void()> auto& otherwise)
{
    if (!step) {
        otherwise();
        return;
    }
    LBasicBlock takesStep = newBlock();
    LBasicBlock doesNotTakeStep = newBlock();
    LBasicBlock done = newBlock();
    branch(step->applies, { normalEdge(takesStep), unlikelyEdge(doesNotTakeStep) });
    m_out.appendTo(takesStep);
    storeField(cell, m_heaps.AOTObject_orderWord, step->orderWordAfter);
    m_out.jump(done);
    m_out.appendTo(doesNotTakeStep);
    otherwise();
    m_out.jump(done);
    m_out.appendTo(done);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
