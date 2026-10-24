/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTErrorMessages.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPendingStars.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "CustomGetterSetter.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "TypeError.h"

namespace JSC { namespace AOT {

// A custom value is computed for the object that has the property.
static void finishSlotOfExtraProperty(Object* object, PropertySlot& slot, bool found)
{
    slot.disableCaching();
    if (found && slot.isCustom()) [[unlikely]]
        slot.setCustom(object, slot.attributes(), slot.customGetter(), slot.customSetter());
}

bool Object::getOwnPropertySlot(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    if (const FieldData* field = object->fieldNamed(object->state(), name)) {
        if (field->isPrivate()) [[unlikely]]
            return false;
        JSValue value = object->readInline(*field);
        if (!value) [[unlikely]]
            value = object->read(*field);
        if (value) [[likely]] {
            if (field->isAccessor()) [[unlikely]]
                slot.setGetterSlot(object, object->attributesOf(*field), uncheckedDowncast<GetterSetter>(value));
            else
                slot.setValue(object, object->attributesOf(*field), value);
            return true;
        }
    } else
        object->state().objects->layouts.didReadAbsentField(globalObject->vm(), object->layout(), name.uid());
    if (object->hasExtraProperties() && slot.internalMethodType() == PropertySlot::InternalMethodType::Get) {
        VM& vm = globalObject->vm();
        if (std::optional<Object*> source = sourceOfPendingBinding(vm, *object, name.uid())) {
            if (!*source)
                return false;
            auto scope = DECLARE_THROW_SCOPE(vm);
            JSValue value = (*source)->get(globalObject, name);
            RETURN_IF_EXCEPTION(scope, false);
            slot.setValue(object, 0, value);
            slot.disableCaching();
            return true;
        }
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return false;
    bool found = extra->methodTable()->getOwnPropertySlot(extra, globalObject, name, slot);
    finishSlotOfExtraProperty(object, slot, found);
    return found;
}

bool Object::getOwnPropertySlotByIndex(JSObject* cell, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    JSObject* extra = uncheckedDowncast<Object>(cell)->extraProperties();
    if (!extra)
        return false;
    bool found = extra->methodTable()->getOwnPropertySlotByIndex(extra, globalObject, index, slot);
    slot.disableCaching();
    return found;
}

void Object::getOwnPropertyNames(JSObject* cell, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    State& state = object->state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = object->layout();
    validateKeyListing(vm, *object);
    std::span<const FieldData> fields = layout.fields();
    JSObject* extra = object->extraProperties();
    if (extra)
        extra->getOwnIndexedPropertyNames(globalObject, names, mode);
    auto extraKeyNames = object->extraKeyNames();
    KeyOrderStorage storage;
    std::span<const uint16_t> order = object->keyOrder(state, storage);
    auto isIncluded = [&](unsigned attributes) {
        return mode == DontEnumPropertiesMode::Include || !(attributes & PropertyAttribute::DontEnum);
    };
    auto addKeysInOrder = [&](bool symbols) {
        forEachPositionedKey(order, layout.numberOfFields, extraKeyNames.size(), [&](uint16_t index) {
            const FieldData& field = fields[index];
            if (field.isPrivate() || !(symbols ? layouts.hasSymbolKey(field) : layouts.hasStringKey(field)))
                return;
            unsigned attributes = field.attributes();
            if (!object->isInSlot(field)) {
                FieldProperty property = findFieldProperty(vm, *object, field);
                if (!property)
                    return;
                attributes = property.attributes;
            }
            if (isIncluded(attributes))
                names.add(Identifier::fromUid(vm, layouts.nameOf(vm, field)));
        }, [&](size_t ordinal) {
            UniquedStringImpl* name = extraKeyNames[ordinal];
            unsigned attributes;
            if (name->isSymbol() == symbols && extra->getDirect(vm, name, attributes) && isIncluded(attributes))
                names.add(Identifier::fromUid(vm, name));
        });
    };
    if (names.includeStringProperties())
        addKeysInOrder(false);
    if (!layout.hasFieldsWithSymbolKeys) {
        if (extra)
            extra->getOwnNonIndexPropertyNames(globalObject, names, mode);
        return;
    }
    auto addExtraKeys = [&](bool symbols) {
        if (!extra)
            return;
        extra->structure()->forEachProperty(vm, [&](const auto& entry) {
            if (entry.key()->isSymbol() == symbols && !PropertyName(entry.key()).isPrivateName() && isIncluded(entry.attributes()))
                names.add(Identifier::fromUid(vm, entry.key()));
            return true;
        });
    };
    if (names.includeStringProperties())
        addExtraKeys(false);
    if (names.includeSymbolProperties()) {
        addKeysInOrder(true);
        addExtraKeys(true);
    }
}

// The store of an object that inherits from this one asks for a setter or a read-only property. A field that was removed as unread was a data property that can be written, and the store passes it as it passes an absent name.
static bool getOwnPropertyDescriptorForStore(JSGlobalObject* globalObject, Object* object, PropertyName name, const PutPropertySlot& slot, PropertyDescriptor& descriptor)
{
    bool isReceiver = slot.thisValue() == JSValue(object);
    if (isReceiver || object->fieldNamed(object->state(), name))
        return object->getOwnPropertyDescriptor(globalObject, name, descriptor);
    JSObject* extra = object->extraProperties();
    return extra && extra->getOwnPropertyDescriptor(globalObject, name, descriptor);
}

static bool ordinarySet(JSGlobalObject* globalObject, Object* object, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyDescriptor ownDescriptor;
    bool isFound = getOwnPropertyDescriptorForStore(globalObject, object, name, slot, ownDescriptor);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isFound) {
        JSValue prototype = object->getPrototypeDirect();
        if (prototype.isObject())
            RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
        ownDescriptor = PropertyDescriptor(jsUndefined(), static_cast<unsigned>(PropertyAttribute::None));
    }
    bool isWritableAboveReceiver = slot.thisValue() != JSValue(object) && ownDescriptor.isDataDescriptor() && ownDescriptor.writable();
    if (isWritableAboveReceiver && !parseIndex(name))
        RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, name, value, slot));
    RELEASE_AND_RETURN(scope, ordinarySetWithOwnDescriptor(globalObject, object, name, value, slot.thisValue(), WTF::move(ownDescriptor), slot.isStrictMode()));
}

// The object is the receiver. The property is an accessor, or it is among the extra properties.
static bool putOutsideDataSlot(JSGlobalObject* globalObject, Object* object, PropertyName name, Object::LeavingProperty existing, JSValue value, bool isStrictMode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned attributes = existing.attributes;
    if (attributes & PropertyAttribute::Accessor)
        RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(existing.valueOrAccessor)->callSetter(globalObject, object, value, isStrictMode));
    if (attributes & PropertyAttribute::ReadOnly)
        return typeError(globalObject, scope, isStrictMode, ReadonlyPropertyWriteError);
    if (attributes & PropertyAttribute::CustomAccessorOrValue) [[unlikely]] {
        if (PutValueFunc setter = uncheckedDowncast<CustomGetterSetter>(existing.valueOrAccessor)->setter()) {
            scope.release();
            bool isStored = setter(object->structure()->realm(), JSValue::encode(object), JSValue::encode(value), name);
            return isStored || attributes & PropertyAttribute::CustomAccessor;
        }
        if (attributes & PropertyAttribute::CustomAccessor)
            return false;
        attributes &= ~PropertyAttribute::CustomValue;
    }
    object->extraProperties()->putDirect(vm, name, value, attributes);
    if (attributes != existing.attributes)
        object->didChangeExtraProperties(vm);
    return true;
}

bool Object::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    return putWithKnownField(cell, globalObject, name, value, slot, object->fieldNamed(object->state(), name));
}

bool Object::putWithKnownField(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot, const FieldData* field)
{
    ASSERT(!field || !field->isPrivate());
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isWritableInSlot = field && !(field->flags & (fieldIsReadOnly | fieldIsAccessor)) && !(object->orderWord() & orderWordIntegrityBits);
    if (isWritableInSlot && slot.thisValue() == JSValue(object) && object->readInline(*field) && object->tryStore(vm, *field, value))
        return true;
    if (slot.thisValue() != JSValue(object) || parseIndex(name)) [[unlikely]]
        RELEASE_AND_RETURN(scope, ordinarySet(globalObject, object, name, value, slot));
    if (field) {
        FieldProperty current = findFieldProperty(vm, *object, *field);
        if (current.isInSlot && !field->isAccessor()) {
            if (current.attributes & PropertyAttribute::ReadOnly)
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            if (object->tryStore(vm, *field, value))
                return true;
            object->moveFieldToExtraProperties(vm, *field, { value, current.attributes }, SlotExitReason::StoreOfOtherType);
            return true;
        }
        if (current) {
            bool isStored = putOutsideDataSlot(globalObject, object, name, { current.valueOrAccessor, current.attributes }, value, slot.isStrictMode());
            RETURN_IF_EXCEPTION(scope, false);
            if (isStored && !(current.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
                object->tryMoveFieldBackToSlot(vm, *field);
            return isStored;
        }
    } else if (object->tryStoreExtraProperty(vm, name, value, { ExtraPropertyStoreFlag::IsKnownNotToBeField }).kind != ExtraPropertyStore::NotHandled)
        return true;
    else if (JSObject* extra = object->extraProperties()) {
        unsigned attributes;
        if (JSValue existing = extra->getDirect(vm, name, attributes))
            RELEASE_AND_RETURN(scope, putOutsideDataSlot(globalObject, object, name, { existing, attributes }, value, slot.isStrictMode()));
    }
    JSValue prototype = object->getPrototypeDirect();
    if (prototype.isObject()) {
        ChainWalk walk = walkChain({ globalObject, object->structure(), object, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, name.uid());
        switch (storeThroughChain(walk)) {
        case StoreThroughChain::CreatesOwnProperty:
            break;
        case StoreThroughChain::CallsSetter:
            slot.setCacheableSetter(walk.holder, walk.offset);
            RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(valueAtOffset(walk.holder, walk.offset))->callSetter(globalObject, object, value, slot.isStrictMode()));
        case StoreThroughChain::Unknown:
        case StoreThroughChain::CallsCustomSetter:
        case StoreThroughChain::IsRefused:
            RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
        }
    }
    if (field && !(object->orderWord() & orderWordIntegrityBits)) {
        // A store creates a data property that can be written, listed and deleted.
        bool fitsSlot = !field->attributes();
        if (fitsSlot && tryCreateField(vm, object->state(), *object, *field, value))
            return true;
        object->moveFieldToExtraProperties(vm, *field, { value, static_cast<unsigned>(PropertyAttribute::None) }, fitsSlot ? SlotExitReason::StoreOfOtherType : SlotExitReason::Redefinition);
        return true;
    }
    RELEASE_AND_RETURN(scope, defineOwnProperty(object, globalObject, name, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), slot.isStrictMode()));
}

bool Object::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    PutPropertySlot slot(cell, shouldThrow);
    return ordinarySet(globalObject, uncheckedDowncast<Object>(cell), Identifier::from(globalObject->vm(), index), value, slot);
}

bool Object::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    State& state = object->state();
    if (const FieldData* field = object->fieldNamed(state, name)) {
        if (field->isPrivate()) [[unlikely]]
            return true;
        FieldProperty current = findFieldProperty(vm, *object, *field);
        if (!current)
            return true;
        if (current.attributes & PropertyAttribute::DontDelete)
            return false;
        if (current.isInSlot) {
            object->deleteField(vm, *field);
            return true;
        }
        JSObject* extra = object->extraProperties();
        bool isDeleted = extra->methodTable()->deleteProperty(extra, globalObject, name, slot);
        slot.disableCaching();
        if (isDeleted) {
            removeFieldFromKeyOrder(vm, state, *object, *field);
            object->didChangeExtraProperties(vm);
            didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Remove, name.uid());
        }
        return isDeleted;
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    auto ordinal = object->extraKeyOrdinal(name);
    bool isDeleted = extra->methodTable()->deleteProperty(extra, globalObject, name, slot);
    slot.disableCaching();
    if (isDeleted) {
        if (ordinal)
            object->removeExtraKeyFromOrder(vm, state, *ordinal);
        object->didChangeExtraProperties(vm);
        object->didRemoveExtraKey(state);
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Remove, name.uid());
    }
    return isDeleted;
}

bool Object::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    VM& vm = globalObject->vm();
    bool isDeleted = extra->methodTable()->deletePropertyByIndex(extra, globalObject, index);
    object->didChangeExtraProperties(vm);
    if (isDeleted)
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Remove, nullptr);
    return isDeleted;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
