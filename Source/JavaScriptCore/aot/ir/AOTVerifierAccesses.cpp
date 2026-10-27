/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
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
    for (LayoutID member : receivers.written()) {
        std::optional<FieldReference> field = findField(program, member, key, BaseChain::Proven);
        if (!field) {
            if (!slot.memberWithoutField)
                slot.memberWithoutField = member;
            continue;
        }
        if (isRemovedField(program, *field) && !slot.memberWithRemovedField)
            slot.memberWithRemovedField = member;
        LayoutID& firstOfForm = fieldAt(program, *field).form.contains(FieldForm::IsAccessor) ? slot.memberWithAccessor : slot.memberWithValue;
        if (!firstOfForm)
            firstOfForm = member;

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
    if (!receivers.all() || receivers.written().isEmpty()) {
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }

    bool readsHalfOfPair = value.opcode == Opcode::GetSlotGetter || value.opcode == Opcode::GetSlotSetter;
    auto verifySlot = [&](PropertyKeyID key) {
        SlotInReceivers slot = slotInReceivers(value.receivers(), key);
        if (slot.memberWithoutField) {
            fail(InternalErrorKind::InvalidGuard, block, id);
            m_errors.last().layout = slot.memberWithoutField;
            return slot;
        }
        LayoutID memberOfOtherForm = readsHalfOfPair ? slot.memberWithValue : slot.memberWithAccessor;
        if (memberOfOtherForm && value.opcode != Opcode::IsSlotFilled) {
            fail(InternalErrorKind::SlotOfOtherForm, block, id);
            m_errors.last().layout = memberOfOtherForm;
            return slot;
        }
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

    bool makesClosure = value.opcode == Opcode::GetDeferredClosureSlot || value.opcode == Opcode::GetDeferredClosureSlotOrUndefined;
    if (makesClosure && m_form < IRForm::Selected && !slot.mayHoldDeferredClosure)
        fail(InternalErrorKind::InvalidGuard, block, id);
}

// The slot of an accessor holds the pair of its functions, which the definition of the accessor makes.
void FunctionVerifier::verifyFormsOfListedFields(BlockID block, ValueID id, const Value& allocation)
{
    const Vector<Field>& fields = m_program.at(allocation.layout()).fields;
    std::span<const uint32_t> listed = allocation.fields();
    std::span<const ValueID> values = allocation.valuesOperands();
    for (size_t index = 0; index < std::min(listed.size(), values.size()); ++index) {
        if (!values[index] || listed[index] >= fields.size() || !fields[listed[index]].form.contains(FieldForm::IsAccessor))
            continue;
        fail(InternalErrorKind::SlotOfOtherForm, block, id);
        m_errors.last().layout = allocation.layout();
        return;
    }
}

void FunctionVerifier::verifyKeyedTail(BlockID block, ValueID id, const Value& value, size_t numberOfKeys)
{
    if (Value::opcodesWithGetterMask.contains(value.opcode) && numberOfKeys > maxVerifiedKeysInAccessorMasks)
        fail(InternalErrorKind::ArgumentCountMismatch, block, id, static_cast<uint32_t>(numberOfKeys));
}

// The operation reads and writes elements of the width that it names.
static bool holdsOnlyViewsOf(const BuiltinTable& builtins, const Type& type, TypedArrayType typedArrayType)
{
    return type.hasOnlyClassObjects() && std::ranges::all_of(type.classes, [&](const ClassPart& part) {
        return builtins.typedArrayTypeOf(part.builtinClass) == typedArrayType;
    });
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
    bool mayLeaveTypeOut = value.opcode == Opcode::GetTypedArrayLength && value.typedArrayType() == NotTypedArray;
    if (Value::opcodesWithTypedArrayType.contains(value.opcode) && !isTypedView(value.typedArrayType()) && !mayLeaveTypeOut) {
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }
    if (Value::opcodesWithTypedArrayType.contains(value.opcode) && !mayLeaveTypeOut && !holdsOnlyViewsOf(m_program.builtins(), typeOf(value.viewOperand()), value.typedArrayType())) {
        fail(InternalErrorKind::OperandTypeMismatch, block, id, value.viewOperand().index());
        return;
    }
    if (Value::opcodesWithHoles.contains(value.opcode) && !hasValidHoles(value)) {
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }
    if (value.opcode == Opcode::NewArray) {
        // Its elements are held to its contents, so they are stored without a test in the storage that it names.
        if (holdsOnlyNumbers(value.arrayStorage()) && (!value.contentsAreProven() || !canStoreWithoutConversion(value.arrayStorage(), m_program.at(value.contents())))) {
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
            return;
        }
        for (ValueID element : value.elementsOperands()) {
            if (!typeOf(element).isJavaScriptValue()) {
                fail(InternalErrorKind::OperandTypeMismatch, block, id, element.index());
                return;
            }
        }
    }
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
    if (value.opcode == Opcode::IsElementPresent && !hasArrayStorageForm(value.arrayStorage())) {
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }
    if (Value::opcodesWithIsPresent.contains(value.opcode) && value.isPresent()) {
        if (value.isInBounds() || !hasArrayStorageForm(value.arrayStorage())) {
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
            return;
        }
    }
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
    if (array.opcode == Opcode::Parameter && analyses().useLists().blockOf(arrayID) != entryBlock)
        return;
    if (array.opcode == Opcode::CallDirect && m_program.at(array.function()).role == FunctionRole::OutlinedBody)
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
