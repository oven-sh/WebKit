/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectInternal.h"

#if ENABLE(AOT)

#include "AOTObjectModel.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

void crashOnImpossibleMutation(VM& vm, const Object& object, ObjectMutation mutation)
{
    dataLogLn("impossible mutation ", static_cast<unsigned>(mutation), " of layout ", object.layout().index);
    crashOnValidationFailure(vm, { ValidationFailureKind::ImpossibleMutation, { }, { }, &object });
}

void crashOnImpossibleMutation(VM& vm, const Object& object, const FieldData& field, ObjectMutation mutation)
{
    dataLogLn("impossible mutation ", static_cast<unsigned>(mutation), " of field ", String(propertyNameOf(vm, object, field)), " of layout ", object.layout().index);
    crashOnValidationFailure(vm, { ValidationFailureKind::ImpossibleMutation, { }, { }, &object });
}

void failIfKeysMustNotBeListed(VM& vm, const Object& object)
{
    const LayoutData& layout = object.layout();
    if (object.state().objects->layouts.everyObjectHadRemovedField(layout))
        crashOnValidationFailure(vm, { ValidationFailureKind::RemovedFieldNamed, { }, { }, &object });
    if (layout.keysAreNeverEnumerated)
        crashOnValidationFailure(vm, { ValidationFailureKind::KeysListed, { }, { }, &object });
}

UniquedStringImpl* propertyNameOf(VM& vm, const Object& object, const FieldData& field)
{
    return object.state().objects->layouts.nameOf(vm, field);
}

FieldProperty findFieldProperty(VM& vm, const Object& object, const FieldData& field)
{
    JSValue value = object.readInline(field);
    if (!value)
        value = object.read(field);
    if (value)
        return { value, object.attributesOf(field), true };
    JSObject* extra = object.extraProperties();
    if (!extra)
        return { };
    unsigned attributes;
    value = extra->getDirect(vm, propertyNameOf(vm, object, field), attributes);
    if (!value)
        return { };
    return { value, field.isPrivate() ? static_cast<unsigned>(PropertyAttribute::None) : attributes, false };
}

bool livesInExtraProperties(VM& vm, const Object& object, const FieldData& field)
{
    JSObject* extra = object.extraProperties();
    return extra && extra->getDirect(vm, propertyNameOf(vm, object, field));
}

bool tryCreateField(VM& vm, State& state, Object& object, const FieldData& field, JSValue value)
{
    ASSERT(!object.isInSlot(field) && !livesInExtraProperties(vm, object, field));
    // The creation is the one store of a field that is stored once, so this does not ask tryStore, which refuses every store to such a field.
    if (field.slot.representation() == SlotRepresentation::DeferredClosure || !state.objects->layouts.fieldAccepts(field, value) || !object.store(vm, field, value))
        return false;
    object.didCreateField(state, field);
    didChangeOwnProperties(vm, object, VM::StructureChainIntegrityEvent::Add, state.objects->layouts.nameOf(vm, field));
    return true;
}

void putProperty(VM& vm, JSObject* holder, PropertyName name, Object::LeavingProperty property)
{
    if (property.attributes & PropertyAttribute::Accessor)
        holder->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(property.valueOrAccessor), property.attributes);
    else
        holder->putDirect(vm, name, property.valueOrAccessor, property.attributes);
}

void didChangeOwnProperties(VM& vm, Object& object, VM::StructureChainIntegrityEvent event, UniquedStringImpl* nameOrNull)
{
    if (object.structure()->mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(event, &object, nameOrNull);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
