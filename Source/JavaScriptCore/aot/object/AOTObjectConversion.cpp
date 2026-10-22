/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

// For the file of counts.
static ASCIILiteral describeExit(SlotExitReason reason, JSValue value)
{
    if (reason != SlotExitReason::StoreOfOtherType)
        return "field left its slot by redefinition"_s;
    if (!value.isCell())
        return value.isUndefinedOrNull() ? "field left its slot for undefined or null"_s : value.isNumber() ? "field left its slot for a number"_s : "field left its slot for a boolean"_s;
    JSCell* cell = value.asCell();
    if (isCompiledClosure(cell))
        return "field left its slot for a compiled closure"_s;
    if (cell->inherits<JSBoundFunction>())
        return "field left its slot for a bound function"_s;
    if (cell->isCallable())
        return "field left its slot for another function"_s;
    if (isCompiledObject(cell))
        return "field left its slot for a compiled object"_s;
    if (cell->isObject())
        return "field left its slot for another object"_s;
    return cell->isString() ? "field left its slot for a string"_s : "field left its slot for another primitive"_s;
}

// A slot has no empty marker, and a layout no word for extra properties, only where flow proves that nothing needs them.
void Object::moveFieldToExtraProperties(VM& vm, const FieldData& field, LeavingProperty leaving, SlotExitReason reason)
{
    willLeaveStateOfBirth(vm, *this, field, mutationOf(reason));
    validateMutation(vm, *this, field, mutationOf(reason));
    RELEASE_ASSERT(field.canLeaveSlot());
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    JSObject* extra = ensureExtraProperties(vm);
    RELEASE_ASSERT(extra);
    State& state = this->state();
    if (!isInKeyOrder(state, field))
        didCreateField(state, field, false);
    at<uint32_t>(offsetOfOrderWord) |= orderWordHasFieldOutsideSlot;
    setPerCellBit(true);
    putProperty(vm, extra, name, leaving);
    clear(field);
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name);
    count(instance(), counterFor(reason));
    state.propertyCaches->countNamed(describeExit(reason, leaving.valueOrAccessor), name);
}

bool Object::tryMoveFieldBackToSlot(VM& vm, const FieldData& field)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return false;
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    unsigned attributes;
    JSValue value = extra->getDirect(vm, name, attributes);
    if (!value)
        return false;
    if ((field.isPrivate() ? 0 : attributes) != attributesOf(field))
        return false;
    if (!tryStore(vm, field, value))
        return false;
    {
        VM::DeletePropertyModeScope ignoreConfigurable(vm, VM::DeletePropertyMode::IgnoreConfigurable);
        DeletePropertySlot slot;
        JSObject::deleteProperty(extra, structure()->realm(), name, slot);
    }
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name);
    return true;
}

bool Object::hasEveryFieldOfBirth() const
{
    State& state = this->state();
    return std::ranges::all_of(layout().fields(), [&](const FieldData& field) {
        if (field.holdsRawValueFromBirth) {
            const void* slot = slotOf(field);
            return slot && !slotHoldsMarker(field.slot.representation(), slot);
        }
        if (!field.isFilledAtBirth || isInSlot(field))
            return true;
        return field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field) && !livesInExtraProperties(vm(), *this, field);
    });
}

void Object::tryFinishBirth()
{
    if (perCellBit() && !(orderWord() & orderWordHasFieldOutsideSlot) && hasEveryFieldOfBirth())
        setPerCellBit(false);
}

void Object::deleteField(VM& vm, const FieldData& field)
{
    willLeaveStateOfBirth(vm, *this, field, ObjectMutation::DeleteField);
    validateMutation(vm, *this, field, ObjectMutation::DeleteField);
    RELEASE_ASSERT(field.canLeaveSlot());
    UniquedStringImpl* name = propertyNameOf(vm, *this, field);
    State& state = this->state();
    clear(field);
    if (field.isFilledAtBirth)
        setPerCellBit(true);
    removeFieldFromKeyOrder(vm, state, *this, field);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Remove, name);
    count(instance(), RuntimeCounterID::FieldsLeftSlotByDelete);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
