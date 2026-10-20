/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTFlowResults.h"
#include "AOTMemoryLocation.h"
#include "AOTObjectWords.h"
#include "AOTPlacement.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "JSCJSValueInlines.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static const PlacedSlot* extensionWordOf(const FieldAccess& access)
{
    return access.slot.needsExtension() ? &access.extensionWord : nullptr;
}

SlotAccessEmitter::SlotAccessEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

const FieldAccess& SlotAccessEmitter::fieldAccessOf(const ValueContext& value) const
{
    return m_placement.fieldAccess(value->fieldAccess());
}

Operand SlotAccessEmitter::failRead(const ValueContext& value, InternalErrorKind kind)
{
    fail(value, kind);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

Operand SlotAccessEmitter::undefinedResult(const ValueContext& value)
{
    switch (value.representation()) {
    case Representation::Void:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
        return emitter<ConstantEmitter>().constantOf(jsUndefined(), value.representation());
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::RawWord:
        break;
    }
    return failRead(value, InternalErrorKind::OpcodeWithoutCode);
}

Operand SlotAccessEmitter::inResultRepresentation(const ValueContext& value, Operand loaded)
{
    return emitter<ConversionEmitter>().inRepresentation(value, loaded, typeOf(value.id()), value.representation());
}

Operand SlotAccessEmitter::loadSlot(CellPointer cell, const FieldAccess& access)
{
    return emitter<SlotEmitter>().loadSlot(cell, access.slot, MemoryLocation::objectField(access.key, access.receivers), extensionWordOf(access));
}

Operand SlotAccessEmitter::loadFilledSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    return emitter<SlotEmitter>().loadSlot(cell, access.slot, MemoryLocation::objectField(access.key, access.receivers), extensionWordOf(access), typeOf(value.id()));
}

Operand SlotAccessEmitter::loadSlotOrUndefined(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    SlotRepresentation representation = access.slot.representation;
    SlotTraits traits = traitsOf(representation);
    Operand loaded = loadSlot(cell, access);
    if (!traits.hasAbsent)
        return inResultRepresentation(value, loaded);
    if (traits.hasUndefined)
        return inResultRepresentation(value, slots.absentAsUndefined(loaded, representation));

    Condition isAbsent = slots.isAbsent(loaded, representation);
    Operand filled = inResultRepresentation(value, loaded);
    return Operand(value.representation(), m_out.select(isAbsent.value(), undefinedResult(value).value, filled.value));
}

Operand SlotAccessEmitter::loadSlotOfMembersWithField(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    LBasicBlock hasField = newBlock();
    LBasicBlock hasNoField = newBlock();
    OperandJoin join(m_out, value.representation());
    emitter<ShapeTestEmitter>().branchOnShapeIn(cell, access.membersWithField, Type::forLayoutSet(access.receivers), { normalEdge(hasField), normalEdge(hasNoField) });

    m_out.appendTo(hasField);
    join.add(loadSlotOrUndefined(value, cell, access));

    m_out.appendTo(hasNoField);
    join.add(undefinedResult(value));
    return join.finish();
}

Operand SlotAccessEmitter::readSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, bool absentIsUndefined)
{
    if (value.representation() == Representation::Void)
        return { };
    if (access.slot.representation == SlotRepresentation::DeferredClosure)
        return failRead(value, InternalErrorKind::SlotAccessNotPlaced);

    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        return absentIsUndefined ? undefinedResult(value) : failRead(value, InternalErrorKind::SlotAccessNotPlaced);
    case FieldAccess::Coverage::SomeMembers:
        return absentIsUndefined ? loadSlotOfMembersWithField(value, cell, access) : inResultRepresentation(value, loadFilledSlot(value, cell, access));
    case FieldAccess::Coverage::EveryMemberHasSlot:
    case FieldAccess::Coverage::EveryMember:
        return absentIsUndefined ? loadSlotOrUndefined(value, cell, access) : inResultRepresentation(value, loadFilledSlot(value, cell, access));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotAccessEmitter::generateGetSlot(const ValueContext& value)
{
    define(value, readSlot(value, in<CellPointer>(value->objectOperand()), fieldAccessOf(value), false));
}

void SlotAccessEmitter::generateGetSlotOrUndefined(const ValueContext& value)
{
    define(value, readSlot(value, in<CellPointer>(value->objectOperand()), fieldAccessOf(value), true));
}

void SlotAccessEmitter::generateGetDeferredClosureSlot(const ValueContext& value)
{
    const FieldAccess& access = fieldAccessOf(value);
    CellPointer cell = in<CellPointer>(value->objectOperand());
    if (access.slot.representation != SlotRepresentation::DeferredClosure) {
        define(value, readSlot(value, cell, access, false));
        return;
    }

    NarrowReference word(m_out.castToInt32(loadSlot(cell, access).as<Pointer>().value()));
    CellPointer closure = either<CellPointer>(isCellReference(word), [&] {
        return decode(word);
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::MaterializeDeferredClosure, { cell, identifierNumber(access.key) }, { .isOnRarePath = true });
    });
    define(value, inResultRepresentation(value, closure));
}

void SlotAccessEmitter::writeSlot(const ValueContext& value, CellPointer cell, const FieldAccess& access, ValueID stored)
{
    SlotEmitter& slots = emitter<SlotEmitter>();
    MemoryLocation location = MemoryLocation::objectField(access.key, access.receivers);
    Operand operand = get(stored);
    const Type& storedType = typeOf(stored);
    bool mayStoreDoubleInExtension = access.slot.representation == SlotRepresentation::Int32OrDouble && !isStoredInCell(access.slot, storedType);
    if (mayStoreDoubleInExtension)
        slots.storeSlot(value, cell, access.slot, location, operand, extensionWordOf(access));
    else
        slots.storeSlot(cell, access.slot, location, operand, extensionWordOf(access), storedType);
    barrierAfterStore(value, cell, operand, storedType);
}

void SlotAccessEmitter::generatePutSlot(const ValueContext& value)
{
    const FieldAccess& access = fieldAccessOf(value);
    if (access.slot.representation == SlotRepresentation::DeferredClosure)
        return;

    // Placement puts a slot out of line only where every PutSlot finds it filled, so the Extension is there and holds its granule.
    ValueID stored = value->valueOperand();
    if (!access.slot.hasStorage() || !(access.slot.isOutOfLine() || isStoredInCell(access.slot, typeOf(stored)))) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    writeSlot(value, in<CellPointer>(value->objectOperand()), access, stored);
}

void SlotAccessEmitter::generatePutSlotOrAdd(const ValueContext& value)
{
    const FieldAccess& access = fieldAccessOf(value);
    if (access.slot.representation == SlotRepresentation::DeferredClosure)
        return;
    if (!access.slot.hasStorage()) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }

    RELEASE_ASSERT(!value->laterBarrierCovers());
    SlotEmitter& slots = emitter<SlotEmitter>();
    CellPointer cell = in<CellPointer>(value->objectOperand());
    ValueID stored = value->valueOperand();
    if (access.slot.isOutOfLine())
        slots.ensureExtensionHolds(value, cell, access.extensionWord, granuleBitOf(access.slot.offset));
    if (!mayCreateListedField(access)) {
        writeSlot(value, cell, access, stored);
        return;
    }

    std::span<const PropertyKeyID> createdKeys = singleElementSpan(access.key);
    auto listFieldIfMissing = [&](KeyOrderPlaces places) {
        LBasicBlock creates = newBlock();
        LBasicBlock done = newBlock();
        Condition isMissing = slots.holdsAbsent(slots.addressOf(cell, access.slot, MemoryLocation::objectField(access.key, access.receivers), extensionWordOf(access)), access.slot.representation);
        branch(isMissing, { unlikelyEdge(creates), normalEdge(done) });
        m_out.appendTo(creates);
        takeKeyOrderStepOr(cell, testKeyOrderStep(cell, access.membersWithField, createdKeys, places), [&] {
            didCreateField(value, cell, access);
        });
        m_out.jump(done);
        m_out.appendTo(done);
    };

    // The key is listed first, while the slot still says whether the field is missing. No code of the program runs before the store.
    // The order word alone says that the field is missing, so the slot is tested only where the step does not apply.
    std::optional<KeyOrderStep> stepTestedFirst;
    if (isExpectedToCreate(value->accessSite()))
        stepTestedFirst = testKeyOrderStep(cell, access.membersWithField, createdKeys, KeyOrderPlaces::MostLikely);
    takeKeyOrderStepOr(cell, stepTestedFirst, [&] {
        listFieldIfMissing(stepTestedFirst ? KeyOrderPlaces::Others : KeyOrderPlaces::All);
    });
    writeSlot(value, cell, access, stored);
}

bool SlotAccessEmitter::isExpectedToCreate(AccessSiteID site) const
{
    return site && !inputs().flow.seesOnlyFinishedObjects(site);
}

bool SlotAccessEmitter::mayCreateListedField(const FieldAccess& access) const
{
    if (!canLeaveSlot(access.slot.representation))
        return false;
    return std::ranges::any_of(m_program.at(access.membersWithField).members, [&](LayoutID member) {
        return !m_placement.layout(member).keysAreNeverEnumerated;
    });
}

// The comparison is with the whole word: an object that is not extensible, or that has properties outside its slots, is left to the run time.
std::optional<KeyOrderStep> SlotAccessEmitter::testKeyOrderStep(CellPointer cell, LayoutSetID receivers, std::span<const PropertyKeyID> createdKeys, KeyOrderPlaces places)
{
    struct Candidate {
        uint32_t shapeWord;
        KeyOrderContinuation continuation;
    };
    static constexpr unsigned maxCandidates = 4;
    const ShapeTable& shapes = m_placement.shapes();
    auto& members = m_program.at(receivers).members;
    Vector<Candidate, maxCandidates> candidates;
    for (LayoutID member : members) {
        if (m_placement.layout(member).keysAreNeverEnumerated)
            continue;
        Vector<FieldIndex, 4> fields;
        for (PropertyKeyID key : createdKeys) {
            if (std::optional<FieldIndex> field = m_program.at(member).findField(key))
                fields.append(*field);
        }
        if (fields.size() != createdKeys.size())
            continue;
        std::span<const KeyOrderContinuation> continuations = m_placement.keyOrders().continuationsCreating(member, fields.span());
        if (places == KeyOrderPlaces::MostLikely)
            continuations = continuations.first(std::min<size_t>(continuations.size(), 1));
        if (places == KeyOrderPlaces::Others)
            continuations = continuations.subspan(std::min<size_t>(continuations.size(), 1));
        for (KeyOrderContinuation continuation : continuations) {
            if (candidates.size() < maxCandidates)
                candidates.append({ AOT::shapeWordOf(shapes.generalShapeOf(member)), continuation });
        }
    }
    if (candidates.isEmpty())
        return std::nullopt;

    // An order is numbered within its shape.
    ShapeTestEmitter& shapeTests = emitter<ShapeTestEmitter>();
    bool hasOneShape = members.size() == 1 && shapes.shapesOf(members[0]).count == 1;
    Word32 orderWord = shapeTests.orderWordOf(cell);
    Word32 shapeWord = hasOneShape ? Word32() : shapeTests.shapeWordOf(cell, receivers);
    KeyOrderStep step;
    for (const Candidate& candidate : candidates) {
        Condition matches = equal(orderWord, word32(static_cast<int32_t>(candidate.continuation.orderWordBefore)));
        if (!hasOneShape)
            matches = logicalAnd(matches, equal(shapeWord, word32(static_cast<int32_t>(candidate.shapeWord))));
        Word32 orderWordAfter = word32(static_cast<int32_t>(candidate.continuation.orderWordAfter));
        step.orderWordAfter = step.applies ? select(matches, orderWordAfter, step.orderWordAfter) : orderWordAfter;
        step.applies = logicalOr(step.applies, matches);
    }
    return step;
}

void SlotAccessEmitter::didCreateField(const ValueContext& value, CellPointer cell, const FieldAccess& access)
{
    const PlacedSlot& slot = access.slot;
    bool isOutOfLine = slot.isOutOfLine();
    uint32_t slotKey = slotKeyFor(slot.offset, isOutOfLine);
    if (slotKey == noSlotKey) {
        callVoidOperation(value, RuntimeOperation::DidCreateField, { cell, identifierNumber(access.key) });
        return;
    }
    if (value.shape() == CodeShape::Compact) {
        callVoidOperation(value, isOutOfLine ? RuntimeOperation::DidCreateFieldOutOfLineAt : RuntimeOperation::DidCreateFieldAt, { cell, word32(slotKey) });
        return;
    }
    if (isOutOfLine) {
        callStub(value, AOT::didCreateField(CreatedFields::OneOutOfLine), { cell.value(), m_out.constInt64(slotKey), m_out.constInt64(static_cast<int64_t>(granuleBitOf(slot.offset))) });
        return;
    }
    callStub(value, AOT::didCreateField(CreatedFields::One), { cell.value(), m_out.constInt64(slotKey) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
