/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinCalls.h"
#include "AOTGuardFactDetails.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTVector.h"
#include <algorithm>
#include <functional>
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxVerifiedKeysInAccessorMasks = std::numeric_limits<uint32_t>::digits;

static_assert((Value::opcodesWithIsPresent | Value::opcodesWithReadsHoleAsNaN | Value::opcodesWithIsInBounds) == Value::opcodesWithIsInBounds, "the rules for isPresent and readsHoleAsNaN ask isInBounds()");

static SlotInReceivers findSlotInReceivers(const Program& program, const LayoutSet& receivers, PropertyKeyID key)
{
    SlotInReceivers slot;
    for (LayoutID member : receivers.members) {
        std::optional<FieldReference> field = findField(program, member, key, BaseChain::Proven);
        if (!field) {
            if (!slot.memberWithoutField)
                slot.memberWithoutField = member;
            continue;
        }
        if (isRemovedField(program, *field) && !slot.memberWithRemovedField)
            slot.memberWithRemovedField = member;

        const FieldFlow& flow = flowOf(program, *field);
        slot.isAlwaysFilled &= flow.occupancy == Occupancy::AlwaysFilled;
        slot.mayHoldDeferredClosure |= flow.location.deferral != ClosureDeferral::Unknown;
    }
    return slot;
}

SlotInReceivers FunctionVerifier::slotInReceivers(LayoutSetID receivers, PropertyKeyID key)
{
    if (!m_slotsInReceivers || TypeTable::isProvisional(receivers) || TypeTable::isProvisional(key))
        return findSlotInReceivers(m_program, m_program.at(receivers), key);

    uint32_t hash = pairIntHash(receivers.index(), key.index());
    auto isWanted = [&](const SlotInReceiversEntry& entry) {
        return entry.receivers == receivers && entry.key == key;
    };
    if (const SlotInReceiversEntry* entry = m_slotsInReceivers->find(hash, isWanted))
        return entry->slot;
    return m_slotsInReceivers->add(hash, SlotInReceiversEntry { receivers, key, findSlotInReceivers(m_program, m_program.at(receivers), key) }, isWanted).slot;
}

static ValueID keyOperandOf(const Value& access)
{
    return OperandPosition::property(access.opcode) ? access.propertyOperand() : access.positionOperand();
}

static bool hasValidHoles(const Value& array)
{
    std::span<const uint32_t> holes = array.holes();
    bool isStrictlyAscending = std::ranges::adjacent_find(holes, std::ranges::greater_equal { }) == holes.end();
    return isStrictlyAscending && (holes.empty() || holes.back() < array.elementsOperands().size() + holes.size());
}

void FunctionVerifier::verifySlotOperation(BlockID block, ValueID id, const Value& value)
{
    const LayoutSet& receivers = m_program.at(value.receivers());
    if (!receivers.isComplete || receivers.members.isEmpty()) {
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }

    auto verifySlot = [&](PropertyKeyID key) {
        SlotInReceivers slot = slotInReceivers(value.receivers(), key);
        if (slot.memberWithoutField) {
            fail(InternalErrorKind::InvalidGuard, block, id);
            m_errors.last().layout = slot.memberWithoutField;
            return slot;
        }
        // While a function is optimized, RemoveUnreadFields has not weakened the record of a field that an operation still names.
        if (slot.memberWithRemovedField && !isOptimizing()) {
            fail(InternalErrorKind::SlotAccessNotPlaced, block, id);
            m_errors.last().layout = slot.memberWithRemovedField;
        }
        return slot;
    };

    if (Value::opcodesWithPlacedKeys.contains(value.opcode)) {
        PayloadListView<PlacedKey> placedKeys = value.placedKeys();
        for (uint32_t index = 0; index < placedKeys.size(); ++index) {
            PlacedKey placedKey = placedKeys[index];
            if (m_form == IRForm::Placed && !placedKey.fieldAccess)
                fail(InternalErrorKind::PlacedPayloadMissing, block, id, index);
            else
                verifySlot(placedKey.key);
            if (hasFailed())
                return;
        }
        return;
    }

    SlotInReceivers slot = verifySlot(value.key());
    if (hasFailed())
        return;

    bool readsFilledSlot = reliesOf(value.opcode).contains(Proof::SlotFilled);
    if (readsFilledSlot && !slot.isAlwaysFilled && !guardProvesSlotFilled(analyses(), value)) {
        fail(InternalErrorKind::InvalidGuard, block, id);
        return;
    }

    // Selection's dispatch on shape copies the read into arms, and the members of one arm may all have an ordinary slot.
    bool makesClosure = value.opcode == Opcode::GetDeferredClosureSlot || value.opcode == Opcode::GetDeferredClosureSlotOrUndefined;
    if (makesClosure && m_form < IRForm::Selected && !slot.mayHoldDeferredClosure)
        fail(InternalErrorKind::InvalidGuard, block, id);
}

void FunctionVerifier::verifyKeyedTail(BlockID block, ValueID id, const Value& value, size_t numberOfKeys)
{
    if (Value::opcodesWithGetterMask.contains(value.opcode) && numberOfKeys > maxVerifiedKeysInAccessorMasks)
        fail(InternalErrorKind::ArgumentCountMismatch, block, id, static_cast<uint32_t>(numberOfKeys));
}

void FunctionVerifier::verifyElementOperation(BlockID block, ValueID id, const Value& value)
{
    if (Value::opcodesWithKeyIsIntegral.contains(value.opcode) && value.keyIsIntegral()) {
        ValueID key = keyOperandOf(value);
        if (!typeOf(key).hasOnlyPrimitives(Primitive::NumberWithoutFraction)) {
            fail(InternalErrorKind::OperandTypeMismatch, block, id, key.index());
            return;
        }
    }
    // The length of a view is read in one way whatever its elements are, so that row may leave the type out.
    bool mayLeaveTypeOut = value.opcode == Opcode::GetTypedArrayLength && value.typedArrayType() == NotTypedArray;
    if (Value::opcodesWithTypedArrayType.contains(value.opcode) && !isTypedView(value.typedArrayType()) && !mayLeaveTypeOut) {
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }
    if (Value::opcodesWithHoles.contains(value.opcode) && !hasValidHoles(value)) {
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }
    // An access inside the length has code for a named storage and an integer, and its store has no path that converts the storage.
    if (Value::opcodesWithIsInBounds.contains(value.opcode) && value.isInBounds()) {
        if (!hasArrayStorageForm(value.arrayStorage())) {
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
            return;
        }
        if (!typeOf(value.positionOperand()).isInteger()) {
            fail(InternalErrorKind::OperandTypeMismatch, block, id, value.positionOperand().index());
            return;
        }
        if (OperandPosition::value(value.opcode) && !canStoreWithoutConversion(value.arrayStorage(), typeOf(value.valueOperand()))) {
            fail(InternalErrorKind::OperandTypeMismatch, block, id, value.valueOperand().index());
            return;
        }
    }
    // The test looks for a hole as a named storage marks it.
    if (value.opcode == Opcode::IsElementPresent && !hasArrayStorageForm(value.arrayStorage())) {
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }
    // A present element answers without the prototypes only in a named storage: no other kind rules out an accessor under an index.
    if (Value::opcodesWithIsPresent.contains(value.opcode) && value.isPresent()) {
        if (value.isInBounds() || !hasArrayStorageForm(value.arrayStorage())) {
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
            return;
        }
    }
    // Only the hole of Double storage is a NaN, and only a position inside the length holds an element or a hole.
    if (Value::opcodesWithReadsHoleAsNaN.contains(value.opcode) && value.readsHoleAsNaN()) {
        if (!value.isInBounds() || value.arrayStorage() != ArrayStorage::Double) {
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
            return;
        }
    }

    if (value.opcode != Opcode::TryPutByValDirect || m_form < IRForm::Selected || value.arrayStorage() == ArrayStorage::Other)
        return;
    ValueID arrayID = analyses().sameObjectThroughCasts(value.arrayOperand());
    const Value& array = m_function.at(arrayID);
    // A parameter of a block inside the function names either of several arrays, and the store tests the storage of the one that it finds.
    if (array.opcode == Opcode::Parameter && analyses().useLists().blockOf(arrayID) != entryBlock)
        return;
    bool isUnderConstruction = array.opcode == Opcode::NewArrayLike || array.opcode == Opcode::NewArrayWithCapacity;
    if (!isUnderConstruction || array.arrayStorage() != value.arrayStorage())
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
}

void FunctionVerifier::verifyStringBuilderOperation(BlockID block, ValueID id, const Value& value)
{
    if (value.opcode == Opcode::StringBuilderAppend) {
        for (ValueID part : value.partsOperands()) {
            if (!isStringBuilderPart(typeOf(part))) {
                fail(InternalErrorKind::OperandTypeMismatch, block, id, part.index());
                return;
            }
        }
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
