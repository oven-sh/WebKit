/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectInternal.h"

#if ENABLE(AOT)

#include "AOTErrorMessages.h"
#include "AOTImage.h"
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

void crashOnLostStateOfBirth(VM& vm, const Object& object, const FieldData& field, ASCIILiteral event)
{
    const LayoutData& layout = object.layout();
    const ImageText& text = vm.aotState().image->text();
    StringID layoutName(layout.name);
    dataLogLn("AOT: ", event, " of field ", String(propertyNameOf(vm, object, field)), " of layout ", layout.index, " (", layoutName && text.hasText(layoutName) ? text.view(layoutName) : StringView("-"_s), "), which compiled code reads without a test");
    crashOnValidationFailure(vm, { ValidationFailureKind::ImpossibleMutation, { }, { }, &object });
}

static thread_local const JSObject* objectListedWithoutOrder;

KeyListingThatShowsNoOrder::KeyListingThatShowsNoOrder(const JSObject* object)
    : m_outer(std::exchange(objectListedWithoutOrder, object))
{
}

KeyListingThatShowsNoOrder::~KeyListingThatShowsNoOrder()
{
    objectListedWithoutOrder = m_outer;
}

void failIfKeysMustNotBeListed(VM& vm, const Object& object)
{
    const LayoutData& layout = object.layout();
    if (object.state().objects->layouts.everyObjectHadRemovedField(layout))
        crashOnValidationFailure(vm, { ValidationFailureKind::RemovedFieldNamed, { }, { }, &object });
    if (layout.keysAreNeverEnumerated && objectListedWithoutOrder != &object)
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
    std::optional<AcceptedValue> accepted = state.objects->layouts.accept(field, value);
    if (!accepted || !object.store(vm, field, *accepted))
        return false;
    object.didCreateField(state, field, !!object.slotOf(field));
    if (field.isFilledAtBirth)
        object.tryFinishBirth();
    if (object.structure()->mayBePrototype()) [[unlikely]]
        didChangeOwnProperties(vm, object, VM::StructureChainIntegrityEvent::Add, state.objects->layouts.nameOf(vm, field));
    return true;
}

void putProperty(VM& vm, JSObject* holder, PropertyName name, Object::LeavingProperty property)
{
    if (property.attributes & PropertyAttribute::Accessor)
        holder->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(property.valueOrAccessor), property.attributes);
    else if (property.attributes & PropertyAttribute::CustomAccessorOrValue)
        holder->putDirectCustomAccessor(vm, name, property.valueOrAccessor, property.attributes);
    else
        holder->putDirect(vm, name, property.valueOrAccessor, property.attributes);
}

void didChangeOwnProperties(VM& vm, Object& object, VM::StructureChainIntegrityEvent event, UniquedStringImpl* nameOrNull)
{
    if (object.structure()->mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(event, &object, nameOrNull);
}

bool interceptsIndexedAccesses(const Object& object)
{
    JSObject* extra = object.extraProperties();
    return extra && extra->structure()->mayInterceptIndexedAccesses();
}

// The extra properties are nobody's prototype, so JSObject::notifyPresenceOfIndexedAccessors() tells nobody on their behalf.
void didChangeIndexedProperties(VM& vm, Object& object, bool interceptedIndexedAccessesBefore)
{
    if (!interceptedIndexedAccessesBefore && object.mayBePrototype() && interceptsIndexedAccesses(object))
        object.structure()->realm()->haveABadTime(vm, &object);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
