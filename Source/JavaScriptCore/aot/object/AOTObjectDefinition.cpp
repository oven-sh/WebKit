/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "TypeError.h"

namespace JSC { namespace AOT {

static Object::LeavingProperty createPropertyFromDescriptor(JSGlobalObject* globalObject, const PropertyDescriptor& descriptor)
{
    unsigned attributes = descriptor.attributes();
    if (!descriptor.isAccessorDescriptor())
        return { descriptor.value() ? descriptor.value() : jsUndefined(), attributes & ~PropertyAttribute::Accessor };
    JSObject* getter = descriptor.getterPresent() ? descriptor.getterObject() : nullptr;
    JSObject* setter = descriptor.setterPresent() ? descriptor.setterObject() : nullptr;
    return { GetterSetter::create(globalObject->vm(), globalObject, getter, setter), (attributes & ~PropertyAttribute::ReadOnly) | PropertyAttribute::Accessor };
}

static bool defineExtraProperty(Object* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto convertAndDefine = [&](ShapeLossReason reason) {
        JSObject* ordinary = object->convertToOrdinaryObject(vm, reason);
        return ordinary->methodTable()->defineOwnProperty(ordinary, globalObject, name, descriptor, shouldThrow);
    };
    JSObject* extra = object->extraProperties();
    bool exists = extra && extra->hasOwnProperty(globalObject, name);
    RETURN_IF_EXCEPTION(scope, false);
    if (!exists && object->isNotExtensible())
        return typeError(globalObject, scope, shouldThrow, NonExtensibleObjectPropertyDefineError);
    if (parseIndex(name) && !object->isFrozen() && (descriptor.isAccessorDescriptor() || !(descriptor.writablePresent() ? descriptor.writable() : exists)))
        RELEASE_AND_RETURN(scope, convertAndDefine(ShapeLossReason::IndexedAccessor));
    if (!exists) {
        extra = object->ensureExtraProperties(vm);
        if (!extra)
            RELEASE_AND_RETURN(scope, convertAndDefine(ShapeLossReason::RepresentationLimit));
        object->willAddExtraProperty(vm, name);
    }
    scope.release();
    bool isDefined = extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow);
    object->didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *object, exists ? VM::StructureChainIntegrityEvent::Change : VM::StructureChainIntegrityEvent::Add, name.uid());
    return isDefined;
}

static bool defineFieldProperty(Object* object, JSGlobalObject* globalObject, const FieldData& field, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    FieldProperty current = findFieldProperty(vm, *object, field);
    if (current && !current.isInSlot) {
        JSObject* extra = object->extraProperties();
        bool isDefined = extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow);
        RETURN_IF_EXCEPTION(scope, false);
        object->tryMoveFieldBackToSlot(vm, field);
        object->didChangeExtraProperties(vm);
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Change, name.uid());
        return isDefined;
    }

    PropertyDescriptor currentDescriptor;
    if (current)
        currentDescriptor.setDescriptor(current.valueOrAccessor, current.attributes);
    bool isValid = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, !object->isNotExtensible(), descriptor, !!current, currentDescriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isValid)
        return false;

    unsigned attributes = current ? descriptor.attributesOverridingCurrent(currentDescriptor) : descriptor.attributes();
    bool fitsSlot = !descriptor.isAccessorDescriptor() && attributes == object->attributesOf(field);
    if (!current) {
        Object::LeavingProperty created = createPropertyFromDescriptor(globalObject, descriptor);
        if (fitsSlot && tryCreateField(vm, object->state(), *object, field, created.valueOrAccessor))
            return true;
        (void)object->moveFieldToExtraProperties(vm, field, created, fitsSlot ? SlotExitReason::StoreOfOtherType : SlotExitReason::Redefinition);
        return true;
    }
    if (fitsSlot) {
        if (object->isFrozen() || !descriptor.value() || object->tryStore(vm, field, descriptor.value()))
            return true;
        (void)object->moveFieldToExtraProperties(vm, field, { descriptor.value(), attributes }, SlotExitReason::StoreOfOtherType);
        return true;
    }

    ShapeStatus status = object->moveFieldToExtraProperties(vm, field, { current.valueOrAccessor, current.attributes }, SlotExitReason::Redefinition);
    JSObject* holder = status == ShapeStatus::Kept ? object->extraProperties() : object;
    scope.release();
    bool isDefined = holder->methodTable()->defineOwnProperty(holder, globalObject, name, descriptor, shouldThrow);
    if (status == ShapeStatus::Kept)
        object->didChangeExtraProperties(vm);
    return isDefined;
}

bool Object::defineOwnProperty(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    const FieldData* field = object->fieldNamed(object->state(), name);
    ASSERT(!field || !field->isPrivate());
    if (field)
        return defineFieldProperty(object, globalObject, *field, name, descriptor, shouldThrow);
    return defineExtraProperty(object, globalObject, name, descriptor, shouldThrow);
}

JSValue Object::getDirectFromNativeCode(VM& vm, PropertyName name, unsigned* attributes)
{
    if (const FieldData* field = fieldNamed(state(), name)) {
        FieldProperty property = findFieldProperty(vm, *this, *field);
        if (property && attributes)
            *attributes = property.attributes;
        return property.valueOrAccessor;
    }
    JSObject* extra = extraProperties();
    if (!extra)
        return { };
    unsigned ignored;
    return extra->getDirect(vm, name, attributes ? *attributes : ignored);
}

static void putFieldDirect(VM& vm, Object& object, const FieldData& field, PropertyName name, Object::LeavingProperty property)
{
    FieldProperty current = findFieldProperty(vm, object, field);
    if (current && !current.isInSlot) {
        putProperty(vm, object.extraProperties(), name, property);
        object.tryMoveFieldBackToSlot(vm, field);
        object.didChangeExtraProperties(vm);
        didChangeOwnProperties(vm, object, VM::StructureChainIntegrityEvent::Change, name.uid());
        return;
    }
    bool fitsSlot = property.attributes == object.attributesOf(field);
    if (fitsSlot && (current ? object.tryStore(vm, field, property.valueOrAccessor) : tryCreateField(vm, object.state(), object, field, property.valueOrAccessor)))
        return;
    (void)object.moveFieldToExtraProperties(vm, field, property, fitsSlot ? SlotExitReason::StoreOfOtherType : SlotExitReason::Redefinition);
}

void Object::putDirectFromNativeCode(VM& vm, PropertyName name, JSValue value, unsigned attributes)
{
    if (attributes & PropertyAttribute::CustomAccessorOrValue) {
        convertToOrdinaryObject(vm, ShapeLossReason::NativeDefinition)->putDirectCustomAccessor(vm, name, value, attributes);
        return;
    }
    if (const FieldData* field = fieldNamed(state(), name)) {
        putFieldDirect(vm, *this, *field, name, { value, attributes });
        return;
    }
    JSObject* extra = ensureExtraProperties(vm);
    if (!extra) {
        putProperty(vm, convertToOrdinaryObject(vm, ShapeLossReason::RepresentationLimit), name, { value, attributes });
        return;
    }
    if (!extra->getDirect(vm, name))
        willAddExtraProperty(vm, name);
    putProperty(vm, extra, name, { value, attributes });
    didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name.uid());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
