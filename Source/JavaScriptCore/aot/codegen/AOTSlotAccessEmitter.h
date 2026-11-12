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
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

JS_EXPORT_PRIVATE LValue appendDistanceOfShape(Output&, const Machine::AbstractHeap& tables, LValue shapeWord, const DistancesByShape&, uint32_t constant);

struct KeyOrderStep {
    Condition applies;
    Word32 orderWordAfter;
};

class SlotAccessEmitter final : public OperationEmitter {
public:
    explicit SlotAccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }
    ~SlotAccessEmitter();

    void generateGetSlot(const ValueContext&);
    bool readsNarrowInteger(const Operation&) const;
    void generateGetSlotOrUndefined(const ValueContext&);
    void generateReadAboveEmptySlot(const ValueContext&);
    void generateGetSlotGetter(const ValueContext&);
    void generateGetSlotSetter(const ValueContext&);
    void generateGetDeferredClosureSlot(const ValueContext& value) { readDeferringSlot(value, false); }
    void generateGetDeferredClosureSlotOrUndefined(const ValueContext& value) { readDeferringSlot(value, true); }
    void generatePutSlot(const ValueContext&);
    void generatePutSlotOrAdd(const ValueContext&);

    CellPointer baseOfSlot(CellPointer, const FieldAccess&);
    CellPointer baseOfSlot(CellPointer, const FieldAccess&, LayoutSetID receivers);
    std::optional<CellPointer> baseOfSlotWhereOneTableTellsShapeAndDistance(CellPointer, const FieldAccess&, LayoutSetID receivers, bool rejectsMarkedObjects, LBasicBlock whereShapeIsNoReceiver);
    LayoutSetID layoutsThatTestAgainstEveryShapeLetThrough(ValueID) const;

    struct CreationByDefinition {
        bool isEmitted { false };
        LBasicBlock leftToRunTime { nullptr };
    };
    CreationByDefinition tryCreateFieldByDefinition(const ValueContext& definition, CellPointer);

    void didCreateField(const ValueContext&, CellPointer, const FieldAccess&, bool isOnRarePath = false);
    void noteComputedKey(ValueID object);
    BlockEdge edgeToCreation(LBasicBlock creates, bool isExpectedToCreate);

    bool mayCreateListedField(const FieldAccess&) const;
    bool nextStoreWritesOrderWordAtOnce(const ValueContext&) const;
    bool mayBeFirstStore(const Operation&) const;
    bool tryPutSlotOrAddByStub(const ValueContext&, CellPointer, const FieldAccess&, Operand stored);
    bool storesIntoLateField(const ValueContext&, const FieldAccess&) const;
    void storeIntoLateField(const ValueContext&, CellPointer, const FieldAccess&, const MemoryLocation&, Operand, const Type& storedType, NOESCAPE const ScopedLambda<void()>& askBeforeCreation);
    uint64_t granuleBitsWithFieldsBehind(const FieldAccess&) const;
    uint64_t granuleBitsWithLaterStores(const ValueContext&, const FieldAccess&) const;
    void markCellIfMarkerReplacesRawValue(CellPointer, const FieldAccess&, ValueID stored);
    void askObjectWhatStandsInWayOfStore(const ValueContext&, CellPointer);
    std::optional<KeyOrderStep> testLikeliestKeyOrderStep(CellPointer, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys);
    std::optional<KeyOrderStep> testNextKeyOrderStep(CellPointer, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys);
    void takeKeyOrderStepOr(CellPointer, const std::optional<KeyOrderStep>&, NOESCAPE const Invocable<void()> auto& otherwise);
    void listCreatedFields(CellPointer, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys, bool isExpectedToCreate, NOESCAPE const Invocable<void()> auto& listIfMissing);

private:
    std::optional<FieldAccess> fieldCreatedByDefinition(const Operation& definition) const;
    std::optional<uint64_t> wordsAfterComputedKeys(const ValueContext&, const FieldAccess&);
    void listFieldAfterComputedKeys(const ValueContext&, CellPointer, uint32_t slotKey, uint64_t wordsBefore);

    ValueID m_objectWithComputedKeys;
    uint32_t m_numberOfComputedKeys { 0 };
    CellPointer newGetterSetter(const ValueContext&, ValueID getter, ValueID setter);

    FieldAccess fieldAccessOf(const ValueContext&, bool isStore) const;
    LayoutSetID setOfMembersWithField(const FieldAccess&);
    Operand readSlot(const ValueContext&, CellPointer, const FieldAccess&, bool absentIsUndefined);
    void askObjectAgainWhatReadAnswered(const ValueContext&, CellPointer, Operand read, bool readsEmptySlotAsUndefined);
    std::optional<CellPointer> loadHalfOfAccessorInSlot(const ValueContext&, ptrdiff_t offsetInGetterSetter);
    void readDeferringSlot(const ValueContext&, bool readsEmptySlotAsUndefined);
    CellPointer makeDeferredClosure(const ValueContext&, CellPointer, const FieldAccess&);
    Operand loadDeferringSlot(const ValueContext&, CellPointer, const FieldAccess&, bool readsEmptySlotAsUndefined);
    bool mayHaveLeftDeferringSlot(const ValueContext&, const FieldAccess&) const;
    Operand loadDeferringSlotOrFieldOutsideIt(const ValueContext&, CellPointer, const FieldAccess&);
    Operand decodeWordOfDeferringSlot(const ValueContext&, NarrowReference);
    void writeSlot(const ValueContext&, CellPointer, const FieldAccess&, ValueID stored);
    bool tryPutSlotOrAddByStub(const ValueContext&, CellPointer, const FieldAccess&, ValueID stored);

    Operand loadSlot(CellPointer, const FieldAccess&);
    Operand loadFilledSlot(const ValueContext&, CellPointer, const FieldAccess&);
    Operand loadSlotOrUndefined(const ValueContext&, CellPointer, const FieldAccess&);
    Operand readIfMemberHasField(const ValueContext&, CellPointer, const FieldAccess&, NOESCAPE const Invocable<Operand()> auto& read);
    Operand inResultRepresentation(const ValueContext&, Operand loaded);
    Operand undefinedResult(const ValueContext&);
    bool receiversAreNeverAllocated(const ValueContext&) const;
    Operand resultWithoutSlot(const ValueContext&);
    Operand resultBehindTestThatNeverPasses(const ValueContext&);
    Operand failRead(const ValueContext&, InternalErrorKind);

    Word32 distanceBehindLowestOffset(CellPointer cell, const FieldAccess& access) { return distanceBehindLowestOffset(cell, access, setOfMembersWithField(access)); }
    Word32 distanceBehindLowestOffset(CellPointer, const FieldAccess&, LayoutSetID setOfMembers);
    std::optional<size_t> findOffsetsByShape(const FieldAccess&, LayoutSetID setOfMembers);

    struct OffsetsByShape {
        DistancesByShape distances;
        uint32_t constant { 0 };
        uint32_t tableSize { 0 };
        uint32_t numberOfSites { 0 };
        LayoutSetID membersLaidIn;
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
    takeKeyOrderStepOr(cell, stepTestedFirst, [&] {
        takeKeyOrderStepOr(cell, stepTestedFirst ? testNextKeyOrderStep(cell, receivers, createdKeys) : std::nullopt, listIfMissing);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
