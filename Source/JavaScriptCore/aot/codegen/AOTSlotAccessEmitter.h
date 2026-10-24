/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct FieldAccess;

// Whether the keys of an object are at a place in an order of the image where the created fields come next, and the order word after them.
struct KeyOrderStep {
    Condition applies;
    Word32 orderWordAfter;
};

// The step that the place of the store remembers, and what it takes to remember another.
struct RememberedKeyOrderStep {
    KeyOrderStep step;
    Integer orderAndShapeWordsBefore;
    uint32_t placePlusOne { 0 };
};

// What is known of an object that the function has allocated and that nothing but the function's own stores by slot has touched since.
struct StateOfNewObject {
    uint32_t orderWord { 0 };
    bool mayHaveExtension { false };
    bool hasLayoutPrototype { true };

    friend bool operator==(const StateOfNewObject&, const StateOfNewObject&) = default;
};

class SlotAccessEmitter final : public OperationEmitter {
public:
    explicit SlotAccessEmitter(CodeGenerator&);

    void generateGetSlot(const ValueContext&);
    void generateGetSlotOrUndefined(const ValueContext&);
    void generateGetSlotGetter(const ValueContext&);
    void generateGetFlattenedSlot(const ValueContext&);
    void generateGetFlattenedSlotOrUndefined(const ValueContext&);
    void generateGetDeferredClosureSlot(const ValueContext&);
    void generateGetDeferredClosureSlotOrUndefined(const ValueContext&);
    void generatePutSlot(const ValueContext&);
    void generatePutSlotOrAdd(const ValueContext&);

    // From where the offset of access.slot leads to the slot of this object: the cell, or an address behind it where the receivers keep the field at different offsets.
    CellPointer baseOfSlot(CellPointer, const FieldAccess&);

    // The address that the offsets of the slots of an object count from: the cell, or for an object that is not made a place in the object that holds it.
    CellPointer cellWithSlotsOf(ValueID object);
    // The same for a value in any form, which may be no object at all: then nobody reads through the address.
    LValue addressOfSlotsOf(ValueID);
    // Gives the object itself for a name behind which nobody knows the outer object any more.
    void makeBorrowedObject(const ValueContext&);
    // Fills the room in the outer object with what the allocation lists, and marks the slot of the field.
    void storeFlattenedObject(const ValueContext&, CellPointer outerObject, LayoutID outerLayout, uint32_t field, ValueID held, bool roomIsZero);

    // Lists the key.
    bool didCreateField(const ValueContext&, CellPointer, const FieldAccess&, bool needsStorage, std::optional<uint32_t> placePlusOne = std::nullopt);

    // An object that a constructor is still filling is expected to get the field from the store.
    bool isExpectedToCreate(AccessSiteID) const;
    bool mayCreateListedField(const FieldAccess&) const;
    void markCellIfMarkerReplacesRawValue(CellPointer, const FieldAccess&, ValueID stored);
    std::optional<KeyOrderStep> testLikeliestKeyOrderStep(CellPointer, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys);
    // In front of the user. Such an object needs no test of what the allocation and the stores have put there.
    std::optional<StateOfNewObject> stateOfNewObjectBefore(const ValueContext& user, ValueID object);
    // The order word of an object of any of the receivers that has the given one, once the fields are created. Nothing where the image has no such place in an order.
    std::optional<uint32_t> orderWordAfterCreating(uint32_t orderWordBefore, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys) const;
    void storeOrderWord(CellPointer, uint32_t);
    void takeKeyOrderStepOr(CellPointer, const std::optional<KeyOrderStep>&, NOESCAPE const Invocable<void()> auto& otherwise);
    // For fields that are missing where no place in an order of the image applies.
    void takeRememberedKeyOrderStepOr(const ValueContext&, CellPointer, NOESCAPE const Invocable<bool(std::optional<uint32_t>)> auto& askRuntime);
    bool takeRememberedKeyOrderStepFirst(const ValueContext&, CellPointer, LayoutSetID receivers, NOESCAPE const Invocable<void()> auto& otherwise);
    // The granule of the slot and those of the stores to the same object that the store leads to: an object that gets one late field tends to get them all.
    uint64_t granuleBitsOfStoresBehind(const ValueContext& store, const FieldAccess&);

    // The address of what the read has read, for whoever wants the address: it follows from the address of the object that was read from.
    std::optional<CellPointer> innerObjectReadBy(const OperationContext& conversion, ValueID read);

private:
    bool flowNamesConstructorOrdersOf(LayoutSetID receivers) const;
    std::optional<RememberedKeyOrderStep> testRememberedKeyOrderStep(const ValueContext&, CellPointer);
    void rememberKeyOrderStep(const ValueContext&, CellPointer, const RememberedKeyOrderStep&);
    std::optional<uint32_t> numberOfStoreThatMayAdd(ValueID);
    std::optional<StateOfNewObject> stateAtBirth(ValueID allocation) const;
    std::optional<StateOfNewObject> stateOfObjectsThatMeet(BlockID, ValueID parameter);
    bool leavesObjectsAsTheyAre(ValueID);
    bool applyStoreBySlot(StateOfNewObject&, const Value& store) const;

    const FieldAccess& fieldAccessOf(const ValueContext&) const;
    Operand readSlot(const ValueContext&, CellPointer, const FieldAccess&, bool absentIsUndefined);
    void readDeferringSlot(const ValueContext&, bool readsEmptySlotAsUndefined);
    Operand loadDeferringSlot(const ValueContext&, CellPointer, const FieldAccess&, bool readsEmptySlotAsUndefined);
    bool mayHaveLeftDeferringSlot(const ValueContext&, const FieldAccess&) const;
    Operand loadDeferringSlotOrFieldOutsideIt(const ValueContext&, CellPointer, const FieldAccess&);
    Operand decodeWordOfDeferringSlot(const ValueContext&, NarrowReference);
    void readBorrowingSlot(const ValueContext&, bool readsEmptySlotAsUndefined);
    void writeSlot(const ValueContext&, CellPointer, CellPointer cellWithSlots, const FieldAccess&, ValueID stored);

    Operand loadSlot(CellPointer, const FieldAccess&);
    Operand loadFilledSlot(const ValueContext&, CellPointer, const FieldAccess&);
    Operand loadSlotOrUndefined(const ValueContext&, CellPointer, const FieldAccess&);
    CellPointer innerObjectOf(const OperationContext&, CellPointer, const FieldAccess&);
    bool isTestedByAccess(ValueID object) const;
    bool isOnlyBaseOfAccesses(ValueID);
    Operand loadSlotOfMembersWithField(const ValueContext&, CellPointer, const FieldAccess&);
    Operand inResultRepresentation(const ValueContext&, Operand loaded);
    Operand undefinedResult(const ValueContext&);
    Operand resultBehindTestThatNeverPasses(const ValueContext&);
    Operand failRead(const ValueContext&, InternalErrorKind);

    Word32 distanceBehindLowestOffset(CellPointer, const FieldAccess&);

    // One constant for all accesses of the function to a name through the same layouts, so that B3 sees the loads from it as one.
    struct OffsetTable {
        PropertyKeyID key;
        LayoutSetID members;
        uint32_t constant;
    };
    Vector<OffsetTable, 4> m_offsetTables;

    // By the read that borrows an object, plus one: where the slots of the object lie.
    LValue addressHeldBy(Operand);
    UncheckedKeyHashMap<uint32_t, LValue> m_cellsWithSlotsOfBorrowedObjects;

    // By value, for the stores that may add a field.
    Vector<uint32_t> m_storesThatMayAddInFront;
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

bool SlotAccessEmitter::takeRememberedKeyOrderStepFirst(const ValueContext& value, CellPointer cell, LayoutSetID receivers, NOESCAPE const Invocable<void()> auto& otherwise)
{
    if (flowNamesConstructorOrdersOf(receivers))
        return false;
    std::optional<RememberedKeyOrderStep> remembered = testRememberedKeyOrderStep(value, cell);
    if (!remembered)
        return false;
    takeKeyOrderStepOr(cell, std::optional<KeyOrderStep>(remembered->step), otherwise);
    return true;
}

void SlotAccessEmitter::takeRememberedKeyOrderStepOr(const ValueContext& value, CellPointer cell, NOESCAPE const Invocable<bool(std::optional<uint32_t>)> auto& askRuntime)
{
    std::optional<RememberedKeyOrderStep> remembered = testRememberedKeyOrderStep(value, cell);
    if (!remembered) {
        askRuntime(std::nullopt);
        return;
    }
    takeKeyOrderStepOr(cell, std::optional<KeyOrderStep>(remembered->step), [&] {
        if (!askRuntime(remembered->placePlusOne))
            rememberKeyOrderStep(value, cell, *remembered);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
