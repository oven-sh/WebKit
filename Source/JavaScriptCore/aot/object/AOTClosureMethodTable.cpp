/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosure.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionNames.h"
#include "AOTHeapHooks.h"
#include "AOTModuleTable.h"
#include "AOTObjectModel.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "FunctionRareData.h"
#include "JSCInlines.h"
#include "PropertyDescriptor.h"

namespace JSC { namespace AOT {

static bool isBoundFunction(const Closure& closure)
{
    return closure.kind().callable() == CallableKind::Bound;
}

// Every function below may make a property of the closure, a read too: `prototype`, `name` and `length` are made when something asks for them.
static ALWAYS_INLINE void validateIsNotInFrame(VM& vm, const Closure* closure)
{
    if (validatesRuntime(vm)) [[unlikely]]
        validateFunctionCellIsNotInFrame(vm, closure);
}

static void clearAllocationOf(Closure* closure)
{
    if (FunctionRareData* rareData = closure->rareData())
        rareData->clear("Store to prototype property of a function");
    ClassTable& classes = closure->state().objects->classes;
    if (const LayoutData* constructed = classes.classConstructedBy(closure->function()))
        classes.didChangeMember(*constructed, ClassSide::Static, closure->vm().propertyNames->prototype.impl());
}

static bool isPlainDataDescriptor(const PropertyDescriptor& descriptor)
{
    if (descriptor.isAccessorDescriptor())
        return false;
    return (!descriptor.writablePresent() || descriptor.writable())
        && (!descriptor.enumerablePresent() || descriptor.enumerable())
        && (!descriptor.configurablePresent() || descriptor.configurable());
}

static bool createsPlainDataProperty(const PropertyDescriptor& descriptor)
{
    return isPlainDataDescriptor(descriptor) && descriptor.writablePresent() && descriptor.enumerablePresent() && descriptor.configurablePresent();
}

// Defining what is already there changes nothing (ValidateAndApplyPropertyDescriptor). The name of a closure that nobody touched is in the image.
// A name with a prefix is not the one that frames show, so a definition of it is still recorded.
static bool restatesInitialName(VM& vm, const Closure& closure, PropertyName name, const PropertyDescriptor& descriptor)
{
    if (name != vm.propertyNames->name || closure.classData() || closure.kind().hasInitialNamePrefix())
        return false;
    FunctionRareData* rareData = closure.rareData();
    if ((rareData && rareData->hasReifiedName()) || closure.getDirect(vm, vm.propertyNames->name))
        return false;
    if (descriptor.isAccessorDescriptor() || !descriptor.value() || !descriptor.value().isString())
        return false;
    if ((descriptor.writablePresent() && descriptor.writable()) || (descriptor.enumerablePresent() && descriptor.enumerable()) || (descriptor.configurablePresent() && !descriptor.configurable()))
        return false;
    auto defined = asString(descriptor.value())->tryGetValue();
    return !defined->isNull() && defined.data == initialNameProperty(closure.function());
}

bool Closure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    const LayoutData* classLayout = closure->classData();
    if (const FieldData* field = classLayout ? StaticFields::findOwnField(*closure, propertyName.uid()) : nullptr) {
        if (JSValue value = closure->state().objects->classes.readStaticField(*field)) {
            slot.setValue(closure, static_cast<unsigned>(PropertyAttribute::None), value);
            return true;
        }
    }
    if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
        return true;
    if (classLayout && closure->isLazyClassObject()) {
        if (propertyName != vm.propertyNames->prototype && propertyName != vm.propertyNames->name && propertyName != vm.propertyNames->length && !closure->state().objects->classes.hasStaticMethodNamed(*classLayout, propertyName.uid()))
            return false;
        closure->ensureClassObject(vm);
        if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
            return true;
    }
    if (propertyName == vm.propertyNames->prototype)
        return closure->ensurePrototypeProperty(vm) && JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot);
    if ((propertyName == vm.propertyNames->hasInstanceSymbol || propertyName == vm.propertyNames->length || propertyName == vm.propertyNames->name) && isBoundFunction(*closure)) [[unlikely]]
        closure->adoptBoundFunctionStructure(vm);
    return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
}

bool Closure::isNameThatBuiltinFunctionsRefuse(VM& vm, UniquedStringImpl* uid)
{
    if (uid->isSymbol())
        return uid == vm.propertyNames->hasInstanceSymbol.impl();
    return uid == vm.propertyNames->bind.impl() || equal(uid, "call"_s) || equal(uid, "apply"_s);
}

bool Closure::refusesOwnProperty(VM& vm, PropertyName name) const
{
    return !name.isPrivateName() && isNameThatBuiltinFunctionsRefuse(vm, name.uid()) && isBuiltinFunction();
}

bool Closure::isBuiltinFunction() const
{
    return ModuleTable(state().instance.tables()).isRuntimeFunction(function());
}

bool Closure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    validateIsNotInFrame(vm, closure);
    if (slot.thisValue() == JSValue(closure) && closure->refusesOwnProperty(vm, name)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        slot.disableCaching();
        // Function.prototype[Symbol.hasInstance] is read-only, so the language itself refuses that store, and says nothing of it in sloppy code.
        bool isTold = slot.isStrictMode() || (slot.context() != PutPropertySlot::ReflectSet && name != vm.propertyNames->hasInstanceSymbol);
        return typeError(globalObject, scope, isTold, assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid()));
    }
    ClassTable& classes = closure->state().objects->classes;
    const LayoutData* classLayout = closure->classData();
    bool isStoreToClassObject = classLayout && slot.thisValue() == JSValue(closure);
    const FieldData* field = isStoreToClassObject ? StaticFields::findOwnField(*closure, name.uid()) : nullptr;
    if (field && StaticFields::isInVariable(*closure, *field)) {
        if (classes.tryStoreStaticField(vm, *field, value))
            return true;
        StaticFields::moveToProperty(vm, *closure, *field);
    }
    closure->prepareForGenericAccess(vm);
    if (classLayout)
        StaticFields::ensureProperties(vm, *closure);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::put(cell, globalObject, name, value, slot);
    if (isStoreToClassObject) {
        // No guard speaks of the value of a property that the class does not declare, so the next store to it may go past this function.
        bool replacesUndeclaredProperty = slot.type() == PutPropertySlot::ExistingProperty && !field && !classes.hasStaticMethodNamed(*classLayout, name.uid());
        if (!replacesUndeclaredProperty)
            slot.disableCaching();
        if (slot.type() != PutPropertySlot::SetterProperty)
            classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
    }
    if (name == vm.propertyNames->prototype) [[unlikely]] {
        slot.disableCaching();
        clearAllocationOf(closure);
    }
    if (name == vm.propertyNames->displayName) [[unlikely]] {
        slot.disableCaching();
        didChangeDisplayName(vm, closure);
    }
    return result;
}

bool Closure::hasOrdinaryDefineOwnProperty(VM& vm, PropertyName name) const
{
    if (classData() || isBoundFunction(*this))
        return false;
    const CommonIdentifiers& names = *vm.propertyNames;
    return name != names.prototype && name != names.name && name != names.length && name != names.displayName;
}

bool Closure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    if (closure->refusesOwnProperty(vm, name)) [[unlikely]]
        return typeError(globalObject, scope, shouldThrow, assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid()));
    if (restatesInitialName(vm, *closure, name, descriptor))
        return true;
    ClassTable& classes = closure->state().objects->classes;
    const LayoutData* classLayout = closure->classData();
    const FieldData* field = classLayout ? StaticFields::findOwnField(*closure, name.uid()) : nullptr;
    if (field && StaticFields::isInVariable(*closure, *field)) {
        if (isPlainDataDescriptor(descriptor) && (!descriptor.value() || classes.tryStoreStaticField(vm, *field, descriptor.value())))
            return true;
        StaticFields::moveToProperty(vm, *closure, *field);
    } else if (field && createsPlainDataProperty(descriptor) && StaticFields::canEnterVariable(vm, *closure, *field)) {
        // Compiled code reads the variable of a field whose definition it could not prove to fit, so the definition by name fills it too.
        if (classes.tryStoreStaticField(vm, *field, descriptor.value() ? descriptor.value() : jsUndefined()))
            return true;
    }
    closure->prepareForGenericAccess(vm);
    if (classLayout)
        StaticFields::ensureProperties(vm, *closure);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (classLayout)
        classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
    if (name == vm.propertyNames->prototype) [[unlikely]]
        clearAllocationOf(closure);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        didChangeDisplayName(vm, closure);
    if (result && name == vm.propertyNames->name && descriptor.value() && descriptor.value().isString()) [[unlikely]] {
        String reported = asString(descriptor.value())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        didDefineNameProperty(vm, closure, reported);
    }
    return result;
}

bool Closure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    validateIsNotInFrame(vm, closure);
    const LayoutData* classLayout = closure->classData();
    const FieldData* field = classLayout ? StaticFields::findOwnField(*closure, name.uid()) : nullptr;
    bool wasInVariable = field && StaticFields::isInVariable(*closure, *field);
    closure->prepareForGenericAccess(vm);
    if (classLayout)
        StaticFields::ensureProperties(vm, *closure);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::deleteProperty(cell, globalObject, name, slot);
    if (result && classLayout) {
        if (wasInVariable)
            StaticFields::didDeleteProperty(vm, *closure, *field);
        else
            closure->state().objects->classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
    }
    if (name == vm.propertyNames->prototype) [[unlikely]]
        clearAllocationOf(closure);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        didChangeDisplayName(vm, closure);
    return result;
}

bool Closure::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    validateIsNotInFrame(vm, closure);
    closure->prepareForGenericAccess(vm);
    return Base::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool Closure::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    closure->prepareForGenericAccess(vm);
    if (const LayoutData* classLayout = closure->classData()) {
        StaticFields::ensureProperties(vm, *closure);
        closure->state().objects->classes.didChangeEveryMember(*classLayout, ClassSide::Static);
    }
    return Base::preventExtensions(object, globalObject);
}

bool Closure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    // A name that the function refuses as its own could be planted one step up.
    if (object->getPrototypeDirect() != prototype && closure->isBuiltinFunction()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return typeError(globalObject, scope, shouldThrow, prototypeChangeOfLockedBuiltinMessage("a built-in function"_s));
    }
    closure->prepareForGenericAccess(vm);
    if (const LayoutData* classLayout = closure->classData(); classLayout && object->getPrototypeDirect() != prototype)
        closure->state().objects->classes.didChangePrototypeLink(*classLayout, ClassSide::Static);
    return Base::setPrototype(object, globalObject, prototype, shouldThrow);
}

// JSObject::putDirect() asks no method table: Error.captureStackTrace(SomeClass) puts `stack` on the class object so. A holder is entered once its class is defined.
void PropertyWalk::didAddPropertyToClosure(VM&, JSObject* object, UniquedStringImpl* uid)
{
    Closure* closure = uncheckedDowncast<Closure>(object);
    const LayoutData* classLayout = closure->classData();
    if (!classLayout)
        return;
    ClassTable& classes = closure->state().objects->classes;
    ClassSide side;
    if (classes.classHeldBy(closure, side) == classLayout && side == ClassSide::Static)
        classes.didChangeMember(*classLayout, ClassSide::Static, uid);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
