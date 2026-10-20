/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTObject.h"
#include "AOTRuntimeCounters.h"
#include "VM.h"

namespace JSC { namespace AOT {

constexpr RuntimeCounterID counterFor(ShapeLossReason reason)
{
    switch (reason) {
    case ShapeLossReason::IndexedAccessor:
        return RuntimeCounterID::ObjectsBecameOrdinaryByIndexedAccessor;
    case ShapeLossReason::NativeDefinition:
        return RuntimeCounterID::ObjectsBecameOrdinaryByNativeDefinition;
    case ShapeLossReason::RepresentationLimit:
        return RuntimeCounterID::ObjectsBecameOrdinaryByRepresentationLimit;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr RuntimeCounterID counterFor(SlotExitReason reason)
{
    switch (reason) {
    case SlotExitReason::Delete:
        return RuntimeCounterID::FieldsLeftSlotByDelete;
    case SlotExitReason::Redefinition:
        return RuntimeCounterID::FieldsLeftSlotByRedefinition;
    case SlotExitReason::StoreOfOtherType:
        return RuntimeCounterID::FieldsLeftSlotByStoreOfOtherType;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr ObjectMutation mutationOf(SlotExitReason reason)
{
    switch (reason) {
    case SlotExitReason::Delete:
        return ObjectMutation::DeleteField;
    case SlotExitReason::Redefinition:
        return ObjectMutation::RedefineProperty;
    case SlotExitReason::StoreOfOtherType:
        return ObjectMutation::StoreOfOtherType;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr ObjectMutation mutationOf(ShapeLossReason reason)
{
    switch (reason) {
    case ShapeLossReason::IndexedAccessor:
    case ShapeLossReason::NativeDefinition:
        return ObjectMutation::RedefineProperty;
    case ShapeLossReason::RepresentationLimit:
        return ObjectMutation::LoseShape;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

[[noreturn]] void crashOnImpossibleMutation(VM&, const Object&, ObjectMutation);

ALWAYS_INLINE void validateMutation(VM& vm, const Object& object, ObjectMutation mutation)
{
    if (vm.aotState().validatesRuntime && object.layout().impossibleMutations().contains(mutation)) [[unlikely]]
        crashOnImpossibleMutation(vm, object, mutation);
}

constexpr uint16_t flagOfFieldsThatCannot(ObjectMutation mutation)
{
    if (mutation == ObjectMutation::DeleteField)
        return fieldIsNeverDeleted;
    if (mutation == ObjectMutation::RedefineProperty)
        return fieldIsNeverRedefined;
    if (mutation == ObjectMutation::StoreOfOtherType)
        return fieldIsNeverStoredWithOtherType;
    return 0;
}

[[noreturn]] void crashOnImpossibleMutation(VM&, const Object&, const FieldData&, ObjectMutation);

ALWAYS_INLINE void validateMutation(VM& vm, const Object& object, const FieldData& field, ObjectMutation mutation)
{
    validateMutation(vm, object, mutation);
    if (vm.aotState().validatesRuntime && field.flags & flagOfFieldsThatCannot(mutation)) [[unlikely]]
        crashOnImpossibleMutation(vm, object, field, mutation);
}

void failIfKeysMustNotBeListed(VM&, const Object&);

// A listing shows which fields the object has and in which order it got them.
ALWAYS_INLINE void validateKeyListing(VM& vm, const Object& object)
{
    if (vm.aotState().validatesRuntime) [[unlikely]]
        failIfKeysMustNotBeListed(vm, object);
}

struct FieldProperty {
    explicit operator bool() const { return !!valueOrAccessor; }

    JSValue valueOrAccessor;
    unsigned attributes { 0 };
    bool isInSlot { false };
};

UniquedStringImpl* propertyNameOf(VM&, const Object&, const FieldData&);
FieldProperty findFieldProperty(VM&, const Object&, const FieldData&);
bool livesInExtraProperties(VM&, const Object&, const FieldData&);
bool tryCreateField(VM&, State&, Object&, const FieldData&, JSValue);
void putProperty(VM&, JSObject* holder, PropertyName, Object::LeavingProperty);
void removeFieldFromKeyOrder(VM&, State&, Object&, const FieldData&);
void didChangeOwnProperties(VM&, Object&, VM::StructureChainIntegrityEvent, UniquedStringImpl* nameOrNull);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
