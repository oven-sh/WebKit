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
struct PlaceOfStoreThatMayAdd;

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
    explicit SlotAccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateGetSlot(const ValueContext&);
    void generateGetSlotOrUndefined(const ValueContext&);
    void generateGetSlotGetter(const ValueContext&);
    void generateGetSlotSetter(const ValueContext&);
    void generateGetDeferredClosureSlot(const ValueContext& value) { readDeferringSlot(value, false); }
    void generateGetDeferredClosureSlotOrUndefined(const ValueContext& value) { readDeferringSlot(value, true); }
    void generatePutSlot(const ValueContext&);
    void generatePutSlotOrAdd(const ValueContext&);

    // From where the offset of access.slot leads to the slot of this object: the cell, or an address behind it where the receivers keep the field at different offsets.
    CellPointer baseOfSlot(CellPointer, const FieldAccess&);

    // Object.defineProperty(object, "key", { .. }) where the object's layout keeps the field with the attributes that the definition creates.
    // Creates the field in its slot, if the object is extensible, is not a prototype and lacks the property. Otherwise the object is unchanged.
    struct CreationByDefinition {
        bool isEmitted { false };
        // Where control goes if the field was not created. Null for an object that the function has just allocated.
        LBasicBlock leftToRunTime { nullptr };
    };
    CreationByDefinition tryCreateFieldByDefinition(const ValueContext& definition, CellPointer);

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
    void takeKeyOrderStepOr(CellPointer, const std::optional<KeyOrderStep>&, NOESCAPE const Invocable<void()> auto& otherwise);
    // Gives the object the order word that it has with the fields, by what is known of a new object or by the step that applies. Otherwise the caller tests the slots, and is told of a remembered step that was tested.
    void listCreatedFields(const ValueContext&, CellPointer, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys, const std::optional<StateOfNewObject>&,
        bool isExpectedToCreate, NOESCAPE const Invocable<void(const std::optional<RememberedKeyOrderStep>&)> auto& listIfMissing);
    // For fields that are missing where no place in an order of the image applies. A step that was tested on the way here is not tested again.
    void takeRememberedKeyOrderStepOr(const ValueContext&, CellPointer, const std::optional<RememberedKeyOrderStep>& stepThatDoesNotApply, NOESCAPE const Invocable<bool(std::optional<uint32_t>)> auto& askRuntime);
    // The granule of the slot and those of the stores to the same object that the store leads to: an object that gets one late field tends to get them all.
    uint64_t granuleBitsOfStoresBehind(const ValueContext& store, const FieldAccess&);

private:
    bool flowNamesConstructorOrdersOf(LayoutSetID receivers) const;
    std::optional<RememberedKeyOrderStep> testRememberedKeyOrderStep(const ValueContext&, CellPointer);
    void rememberKeyOrderStep(const ValueContext&, CellPointer, const RememberedKeyOrderStep&);
    std::optional<PlaceOfStoreThatMayAdd> placeOfStoreThatMayAdd(ValueID);
    std::optional<StateOfNewObject> stateAtBirth(ValueID allocation) const;
    std::optional<StateOfNewObject> stateOfObjectsThatMeet(BlockID, ValueID parameter);
    bool leavesObjectsAsTheyAre(ValueID);
    bool applyStoreBySlot(StateOfNewObject&, const Value& store) const;
    std::optional<FieldAccess> fieldCreatedByDefinition(const Value& definition) const;
    bool slotTakesAccessor(const Field&, SlotRepresentation, ValueID getter, ValueID setter) const;
    CellPointer newGetterSetter(const ValueContext&, ValueID getter, ValueID setter);

    const FieldAccess& fieldAccessOf(const ValueContext&) const;
    Operand readSlot(const ValueContext&, CellPointer, const FieldAccess&, bool absentIsUndefined);
    // Nothing where the operation has got its result without a read.
    std::optional<CellPointer> loadHalfOfAccessorInSlot(const ValueContext&, ptrdiff_t offsetInGetterSetter);
    void readDeferringSlot(const ValueContext&, bool readsEmptySlotAsUndefined);
    CellPointer makeDeferredClosure(const ValueContext&, CellPointer, const FieldAccess&);
    Operand loadDeferringSlot(const ValueContext&, CellPointer, const FieldAccess&, bool readsEmptySlotAsUndefined);
    bool mayHaveLeftDeferringSlot(const ValueContext&, const FieldAccess&) const;
    Operand loadDeferringSlotOrFieldOutsideIt(const ValueContext&, CellPointer, const FieldAccess&);
    Operand decodeWordOfDeferringSlot(const ValueContext&, NarrowReference);
    void writeSlot(const ValueContext&, CellPointer, const FieldAccess&, ValueID stored);

    Operand loadSlot(CellPointer, const FieldAccess&);
    Operand loadFilledSlot(const ValueContext&, CellPointer, const FieldAccess&);
    Operand loadSlotOrUndefined(const ValueContext&, CellPointer, const FieldAccess&);
    Operand readIfMemberHasField(const ValueContext&, CellPointer, const FieldAccess&, NOESCAPE const Invocable<Operand()> auto& read);
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

void SlotAccessEmitter::listCreatedFields(const ValueContext& value, CellPointer cell, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys,
    const std::optional<StateOfNewObject>& newObject, bool isExpectedToCreate, NOESCAPE const Invocable<void(const std::optional<RememberedKeyOrderStep>&)> auto& listIfMissing)
{
    if (std::optional<uint32_t> orderWordAfter = newObject ? orderWordAfterCreating(newObject->orderWord, receivers, createdKeys) : std::nullopt) {
        storeField(cell, m_heaps.AOTObject_orderWord, word32(static_cast<int32_t>(*orderWordAfter)));
        return;
    }
    std::optional<KeyOrderStep> stepTestedFirst;
    std::optional<RememberedKeyOrderStep> remembered;
    if (isExpectedToCreate) {
        if (!flowNamesConstructorOrdersOf(receivers))
            remembered = testRememberedKeyOrderStep(value, cell);
        stepTestedFirst = remembered ? std::optional<KeyOrderStep>(remembered->step) : testLikeliestKeyOrderStep(cell, receivers, createdKeys);
    }
    takeKeyOrderStepOr(cell, stepTestedFirst, [&] {
        listIfMissing(remembered);
    });
}

void SlotAccessEmitter::takeRememberedKeyOrderStepOr(const ValueContext& value, CellPointer cell, const std::optional<RememberedKeyOrderStep>& stepThatDoesNotApply, NOESCAPE const Invocable<bool(std::optional<uint32_t>)> auto& askRuntime)
{
    auto askRuntimeAndRemember = [&](const RememberedKeyOrderStep& remembered) {
        if (!askRuntime(remembered.placePlusOne))
            rememberKeyOrderStep(value, cell, remembered);
    };
    if (stepThatDoesNotApply) {
        askRuntimeAndRemember(*stepThatDoesNotApply);
        return;
    }
    std::optional<RememberedKeyOrderStep> remembered = testRememberedKeyOrderStep(value, cell);
    if (!remembered) {
        askRuntime(std::nullopt);
        return;
    }
    takeKeyOrderStepOr(cell, std::optional<KeyOrderStep>(remembered->step), [&] {
        askRuntimeAndRemember(*remembered);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
