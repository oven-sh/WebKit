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
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "TopExceptionScope.h"
#include "TypeError.h"

namespace JSC { namespace AOT {

static void setOrdinaryObjectIntegrity(JSGlobalObject* globalObject, JSObject* ordinary, Object::Integrity integrity)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    switch (integrity) {
    case Object::Integrity::NotExtensible:
        ordinary->methodTable()->preventExtensions(ordinary, globalObject);
        break;
    case Object::Integrity::Sealed:
        objectConstructorSeal(globalObject, ordinary);
        break;
    case Object::Integrity::Frozen:
        objectConstructorFreeze(globalObject, ordinary);
        break;
    }
    scope.assertNoException();
}

static bool indexedPropertiesHaveIntegrity(JSGlobalObject* realm, JSObject* ordinary, Object::Integrity integrity)
{
    if (!hasIndexedProperties(ordinary->indexingType()))
        return true;
    VM& vm = realm->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyNameArrayBuilder indices(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    ordinary->getOwnIndexedPropertyNames(realm, indices, DontEnumPropertiesMode::Include);
    for (const Identifier& index : indices) {
        PropertyDescriptor descriptor;
        bool isPresent = ordinary->getOwnPropertyDescriptor(realm, index, descriptor);
        scope.assertNoException();
        if (!isPresent)
            continue;
        if (descriptor.configurable())
            return false;
        if (integrity == Object::Integrity::Frozen && descriptor.isDataDescriptor() && descriptor.writable())
            return false;
    }
    return true;
}

void Object::setIntegrity(JSGlobalObject* globalObject, Integrity integrity)
{
    VM& vm = globalObject->vm();
    uint32_t bits = orderWordIsNotExtensible;
    if (integrity != Integrity::NotExtensible)
        bits |= orderWordIsSealed;
    if (integrity == Integrity::Frozen)
        bits |= orderWordIsFrozen;
    if ((orderWord() & bits) == bits)
        return;
    validateMutation(vm, *this, ObjectMutation::ChangeIntegrity);
    bool startedInterceptingIndexedAccesses = false;
    if (JSObject* extra = extraProperties()) {
        bool interceptedIndexedAccesses = extra->structure()->mayInterceptIndexedAccesses();
        setOrdinaryObjectIntegrity(globalObject, extra, integrity);
        didChangeExtraProperties(vm);
        startedInterceptingIndexedAccesses = !interceptedIndexedAccesses && extra->structure()->mayInterceptIndexedAccesses();
    }
    at<uint32_t>(offsetOfOrderWord) |= bits;
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, nullptr);
    if (startedInterceptingIndexedAccesses && mayBePrototype())
        structure()->realm()->haveABadTime(vm, this);
}

bool Object::testIntegrity(VM& vm, Integrity integrity) const
{
    if (!isNotExtensible())
        return false;
    if (integrity == Integrity::NotExtensible)
        return true;
    bool testsFrozen = integrity == Integrity::Frozen;
    if (testsFrozen ? isFrozen() : isSealed())
        return true;
    for (const FieldData& field : layout().fields()) {
        if (!field.isPrivate() && read(field))
            return false;
    }
    JSObject* extra = extraProperties();
    if (!extra)
        return true;
    if (!(testsFrozen ? extra->isFrozen(vm) : extra->isSealed(vm)))
        return false;
    return indexedPropertiesHaveIntegrity(structure()->realm(), extra, integrity);
}

bool Object::preventExtensions(JSObject* cell, JSGlobalObject* globalObject)
{
    uncheckedDowncast<Object>(cell)->setIntegrity(globalObject, Integrity::NotExtensible);
    return true;
}

bool Object::isExtensible(JSObject* cell, JSGlobalObject*)
{
    return !uncheckedDowncast<Object>(cell)->isNotExtensible();
}

void Object::setPrototypeKeepingShape(VM& vm, JSValue prototype)
{
    validateMutation(vm, *this, ObjectMutation::ChangePrototype);
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    bool chainAlreadyMayInterceptIndexedAccesses = mayBePrototype() && anyObjectInChainMayInterceptIndexedAccesses();
    JSCell::setStructure(vm, state().objects->structures.withPrototype(vm, compiledStructure(), prototype));
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Prototype, nullptr);
    if (mayBePrototype() && !chainAlreadyMayInterceptIndexedAccesses && anyObjectInChainMayInterceptIndexedAccesses())
        structure()->realm()->haveABadTime(vm, this);
}

void Object::setPrototypeAtCreation(VM& vm, JSValue prototype)
{
    ASSERT(!mayBePrototype());
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    JSCell::setStructure(vm, state().objects->structures.withPrototype(vm, compiledStructure(), prototype));
}

bool Object::setPrototype(JSObject* cell, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (object->getPrototypeDirect() == prototype)
        return true;
    if (object->isNotExtensible())
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    for (JSValue next = prototype; next.isObject() && asObject(next)->type() != ProxyObjectType; next = asObject(next)->getPrototypeDirect()) {
        if (next == JSValue(object))
            return typeError(globalObject, scope, shouldThrow, "cyclic __proto__ value"_s);
    }
    object->setPrototypeKeepingShape(vm, prototype);
    return true;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
