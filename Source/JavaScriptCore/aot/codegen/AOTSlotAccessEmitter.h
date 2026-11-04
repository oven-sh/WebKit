/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTShapeTest.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

// An Int32. `constant`: the table among the constants of the procedure, where the form has one. `tables`: AOTImage_tables.
JS_EXPORT_PRIVATE LValue appendDistanceOfShape(Output&, const Machine::AbstractHeap& tables, LValue shapeWord, const DistancesByShape&, uint32_t constant);

// Whether the keys of an object are at a place in an order of the image where the created fields come next, and the order word after them.
struct KeyOrderStep {
    Condition applies;
    Word32 orderWordAfter;
};

class SlotAccessEmitter final : public OperationEmitter {
public:
    explicit SlotAccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }
    ~SlotAccessEmitter();

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
    // What the record of the definition answers is not tested: orderWordAfter, objectIsNoPrototype.
    struct CreationByDefinition {
        bool isEmitted { false };
        // Where control goes if the field was not created. Null: nothing was tested.
        LBasicBlock leftToRunTime { nullptr };
    };
    CreationByDefinition tryCreateFieldByDefinition(const ValueContext& definition, CellPointer);

    // Lists the key.
    void didCreateField(const ValueContext&, CellPointer, const FieldAccess&, bool isOnRarePath = false);
    // To the block that calls for a field that is missing. Where the store expects none the call is rare, so a function without another call has no frame on the way that is taken.
    BlockEdge edgeToCreation(LBasicBlock creates, bool isExpectedToCreate);

    bool mayCreateListedField(const FieldAccess&) const;
    bool tryPutSlotOrAddByStub(const ValueContext&, CellPointer, const FieldAccess&, Operand stored);
    // Of a slot out of line: its granule, and those of the fields out of line that the likeliest key order of every member lists behind the field.
    // `request.user = user; request.session = session;`: the store that makes the Extension makes room for them all.
    uint64_t granuleBitsWithFieldsBehind(const FieldAccess&) const;
    void markCellIfMarkerReplacesRawValue(CellPointer, const FieldAccess&, ValueID stored);
    std::optional<KeyOrderStep> testLikeliestKeyOrderStep(CellPointer, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys);
    void takeKeyOrderStepOr(CellPointer, const std::optional<KeyOrderStep>&, NOESCAPE const Invocable<void()> auto& otherwise);
    // Gives the object the order word that it has with the fields, by the step that applies. Otherwise the caller tests the slots. `isExpectedToCreate` is the flag of the store.
    void listCreatedFields(CellPointer, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys, bool isExpectedToCreate, NOESCAPE const Invocable<void()> auto& listIfMissing);

private:
    std::optional<FieldAccess> fieldCreatedByDefinition(const Operation& definition) const;
    CellPointer newGetterSetter(const ValueContext&, ValueID getter, ValueID setter);

    FieldAccess fieldAccessOf(const ValueContext&, bool isStore) const;
    LayoutSetID setOfMembersWithField(const FieldAccess&);
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
    // For a store that needs storage and may list the key of the field. False: nothing was appended.
    bool tryPutSlotOrAddByStub(const ValueContext&, CellPointer, const FieldAccess&, ValueID stored);

    Operand loadSlot(CellPointer, const FieldAccess&);
    Operand loadFilledSlot(const ValueContext&, CellPointer, const FieldAccess&);
    Operand loadSlotOrUndefined(const ValueContext&, CellPointer, const FieldAccess&);
    Operand readIfMemberHasField(const ValueContext&, CellPointer, const FieldAccess&, NOESCAPE const Invocable<Operand()> auto& read);
    Operand inResultRepresentation(const ValueContext&, Operand loaded);
    Operand undefinedResult(const ValueContext&);
    bool receiversAreNeverAllocated(const ValueContext&) const;
    // Of an access that finds no slot to go to. A store has no result and ignores it.
    Operand resultWithoutSlot(const ValueContext&);
    Operand resultBehindTestThatNeverPasses(const ValueContext&);
    Operand failRead(const ValueContext&, InternalErrorKind);

    Word32 distanceBehindLowestOffset(CellPointer, const FieldAccess&);

    // One for all accesses of the function to a name through the same layouts. The bytes of a table have gone to the constant.
    struct OffsetsByShape {
        DistancesByShape distances;
        uint32_t constant { 0 };
        uint32_t tableSize { 0 };
        uint32_t numberOfSites { 0 };
    };
    Vector<OffsetsByShape, 2> m_offsetsByShape;
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_offsetsByShapeOfKeyAndMembers;
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

void SlotAccessEmitter::listCreatedFields(CellPointer cell, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys, bool isExpectedToCreate, NOESCAPE const Invocable<void()> auto& listIfMissing)
{
    std::optional<KeyOrderStep> stepTestedFirst;
    if (isExpectedToCreate)
        stepTestedFirst = testLikeliestKeyOrderStep(cell, receivers, createdKeys);
    takeKeyOrderStepOr(cell, stepTestedFirst, listIfMissing);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
