/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003-2024 Apple Inc. All rights reserved.
 *  Copyright (C) 2007 Eric Seidel (eric@webkit.org)
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "JSObject.h"

#include "AllocationFailureMode.h"
#include "BlockDirectory.h"
#include "ClonedArguments.h"
#include "CompleteSubspace.h"
#include "CustomGetterSetter.h"
#include "ErrorInstance.h"
#include "Exception.h"
#include "GCDeferralContextInlines.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "ImmutablePropertyNames.h"
#include "JSCInlines.h"
#include "IndexingTypeInlines.h"
#include "JSCellButterfly.h"
#include "JSCustomGetterFunction.h"
#include "JSCustomSetterFunction.h"
#include "JSFunction.h"
#include "JSGlobalProxy.h"
#include "JSObjectVisitInlines.h"
#include "Lookup.h"
#include "MarkedSpace.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "PropertyRun.h"
#include "ProxyObject.h"
#include "RegExpObject.h"
#include "ResourceExhaustion.h"
#include "StringObject.h"
#include "SymbolTable.h"
#include "TopExceptionScope.h"
#include "TypeError.h"
#include "VMInlines.h"
#include "VMTrapsInlines.h"
#include <wtf/Assertions.h>
#include <wtf/text/MakeString.h>

#include "AOTCompiledCallee.h"
#include "AOTMembersLeftOut.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeHooks.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// We keep track of the size of the last array after it was grown. We use this
// as a simple heuristic for as the value to grow the next array from size 0.
// This value is capped by the constant FIRST_VECTOR_GROW defined in
// ArrayConventions.h.
static unsigned lastArraySize = 0;

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSObject);
STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSObjectWithButterfly);
STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSFinalObject);

const ASCIILiteral NonExtensibleObjectPropertyDefineError { "Attempting to define property on object that is not extensible."_s };
const ASCIILiteral ImmutablePropertyDefineError { "Attempting to define property on object with immutable properties."_s };

// Who asks for the change: an operation of the language, which in sloppy code goes on in silence where it is refused, or somebody who takes false for an answer, as a function of Reflect does.
enum class RefusalIs : uint8_t { ToldToOperation, Answered };

struct RefusalOfOwner {
    bool alwaysThrows { false };
    String message;
};

// An object whose every property is immutable has no list, and so no owner of one to ask.
static RefusalOfOwner refusalOfOwner(JSGlobalObject* globalObject, JSObject* object, UniquedStringImpl* uid, bool isDeletion)
{
    if (ImmutablePropertyNames* names = object->structure()->listedImmutablePropertyNames())
        return { names->refusalAlwaysThrows(), isDeletion ? names->messageOfRefusedDeletion(uid) : names->messageOfRefusal(uid) };
#if ENABLE(AOT)
    String message = AOT::RuntimeHooks::messageOfRefusalByLockedObject(globalObject, object, uid, isDeletion ? AOT::RefusedChange::Deletion : AOT::RefusedChange::Store);
    return { !message.isNull(), WTF::move(message) };
#else
    UNUSED_PARAM(globalObject);
    return { };
#endif
}

static bool refuseChangeOfImmutableProperty(JSGlobalObject* globalObject, ThrowScope& scope, bool shouldThrow, RefusalIs refusalIs, JSObject* object, PropertyName propertyName, ASCIILiteral message)
{
#if ENABLE(AOT)
    AOT::RuntimeHooks::didRefuseChange(globalObject, object, propertyName.uid(), message == ReadonlyPropertyWriteError ? AOT::RefusedChange::Store : AOT::RefusedChange::Definition);
#endif
    RefusalOfOwner refusal = refusalOfOwner(globalObject, object, propertyName.uid(), false);
    if (!shouldThrow && (refusalIs == RefusalIs::Answered || !refusal.alwaysThrows))
        return false;
    if (refusal.message.isNull())
        return typeError(globalObject, scope, true, message);
    return typeError(globalObject, scope, true, refusal.message);
}

// The language refuses a store that finds, on the object or above it, a property that is read-only or an accessor without a setter, whatever else stands in the way.
static bool isStoreRefusedByLanguage(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = object->getPropertySlot(globalObject, propertyName, slot);
    slot.disallowVMEntry.reset();
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return false;
    }
    if (!isFound)
        return false;
    if (slot.isAccessor())
        return slot.getterSetter()->isSetterNull();
    return slot.attributes() & PropertyAttribute::ReadOnly;
}
const ASCIILiteral ReadonlyPropertyWriteError { "Attempted to assign to readonly property."_s };
const ASCIILiteral ReadonlyPropertyChangeError { "Attempting to change value of a readonly property."_s };
const ASCIILiteral UnableToDeletePropertyError { "Unable to delete property."_s };
const ASCIILiteral UnconfigurablePropertyChangeAccessMechanismError { "Attempting to change access mechanism for an unconfigurable property."_s };
const ASCIILiteral UnconfigurablePropertyChangeConfigurabilityError { "Attempting to change configurable attribute of unconfigurable property."_s };
const ASCIILiteral UnconfigurablePropertyChangeEnumerabilityError { "Attempting to change enumerable attribute of unconfigurable property."_s };
const ASCIILiteral UnconfigurablePropertyChangeWritabilityError { "Attempting to change writable attribute of unconfigurable property."_s };
const ASCIILiteral PrototypeValueCanOnlyBeAnObjectOrNullTypeError { "Prototype value can only be an object or null"_s };

const ClassInfo JSObject::s_info = { "Object"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(JSObject) };

const ClassInfo JSObjectWithButterfly::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSObjectWithButterfly) };

const ClassInfo JSFinalObject::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSFinalObject) };

size_t JSObject::estimatedSize(JSCell* cell, VM& vm)
{
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    size_t butterflyOutOfLineSize = thisObject->butterfly() ? thisObject->structure()->outOfLineSize() : 0;
    return Base::estimatedSize(cell, vm) + butterflyOutOfLineSize;
}

template<typename Visitor>
void JSObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    typename Visitor::DefaultMarkingViolationAssertionScope assertionScope(visitor);

    JSCell::visitChildren(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSObject);

template<typename Visitor>
void JSObjectWithButterfly::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSObjectWithButterfly* thisObject = uncheckedDowncast<JSObjectWithButterfly>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    typename Visitor::DefaultMarkingViolationAssertionScope assertionScope(visitor);

    JSCell::visitChildren(thisObject, visitor);

    thisObject->visitButterfly(visitor);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSObjectWithButterfly);

void JSObject::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    Base::analyzeHeap(cell, analyzer);

    Structure* structure = thisObject->structure();
    for (const auto& entry : structure->getPropertiesConcurrently()) {
        JSValue toValue = thisObject->getDirect(entry.offset());
        if (toValue && toValue.isCell())
            analyzer.analyzePropertyNameEdge(thisObject, toValue.asCell(), entry.key());
    }

    Butterfly* butterfly = thisObject->butterfly();
    if (butterfly) {
        WriteBarrier<Unknown>* data = nullptr;
        uint32_t count = 0;

        switch (thisObject->indexingType()) {
        case ALL_CONTIGUOUS_INDEXING_TYPES:
            data = butterfly->contiguous().data();
            count = butterfly->publicLength();
            break;
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            data = butterfly->arrayStorage()->m_vector;
            count = butterfly->arrayStorage()->vectorLength();
            break;
        default:
            break;
        }

        for (uint32_t i = 0; i < count; ++i) {
            JSValue toValue = data[i].get();
            if (toValue && toValue.isCell())
                analyzer.analyzeIndexEdge(thisObject, toValue.asCell(), i);
        }
    }
}

template<typename Visitor>
void JSFinalObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSFinalObject* thisObject = uncheckedDowncast<JSFinalObject>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    typename Visitor::DefaultMarkingViolationAssertionScope assertionScope(visitor);
    
    JSCell::visitChildren(thisObject, visitor);

    visitFields(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSFinalObject);

String JSObject::calculatedClassName(JSObject* object)
{
    String constructorFunctionName;
    auto* globalObject = object->realmMayBeNull();
    if (!globalObject)
        return object->structure()->classInfoForCells()->className;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    // Check for a display name of obj.constructor.
    // This is useful to get `Foo` for the `(class Foo).prototype` object.
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    auto getOwnConstructor = [&] {
#if ENABLE(AOT)
        // The properties of a compiled object lie where only its own hook finds them.
        if (object->structure()->isCompiledStructure())
            return object->methodTable()->getOwnPropertySlot(object, globalObject, vm.propertyNames->constructor, slot);
#endif
        return object->getOwnPropertySlot(object, globalObject, vm.propertyNames->constructor, slot);
    };
    if (getOwnConstructor()) {
        EXCEPTION_ASSERT(!scope.exception());
        if (slot.isValue()) {
            if (JSObject* ctorObject = dynamicDowncast<JSObject>(slot.getValue(globalObject, vm.propertyNames->constructor))) {
                if (JSFunction* constructorFunction = dynamicDowncast<JSFunction>(ctorObject))
                    constructorFunctionName = constructorFunction->calculatedDisplayName(vm);
                else if (InternalFunction* constructorFunction = dynamicDowncast<InternalFunction>(ctorObject))
                    constructorFunctionName = constructorFunction->calculatedDisplayName(vm);
                else if (String name = isCompiledCalleeType(ctorObject->type()) ? getCalculatedDisplayName(vm, ctorObject) : String(); !name.isEmpty())
                    constructorFunctionName = name;
            }
        }
    }

    EXCEPTION_ASSERT(!scope.exception() || constructorFunctionName.isNull());
    if (scope.exception()) [[unlikely]]
        scope.clearException();

    // Get the display name of obj.__proto__.constructor.
    // This is useful to get `Foo` for a `new Foo` object.
    if (constructorFunctionName.isNull()) {
        if (!object->structure()->typeInfo().overridesGetPrototype()) [[likely]] {
            JSValue protoValue = object->getPrototypeDirect();
            if (protoValue.isObject()) {
                JSObject* protoObject = asObject(protoValue);
                PropertySlot slot(protoValue, PropertySlot::InternalMethodType::VMInquiry, &vm);
                if (protoObject->getPropertySlot(globalObject, vm.propertyNames->constructor, slot)) {
                    EXCEPTION_ASSERT(!scope.exception());
                    if (slot.isValue()) {
                        if (JSObject* ctorObject = dynamicDowncast<JSObject>(slot.getValue(globalObject, vm.propertyNames->constructor))) {
                            if (JSFunction* constructorFunction = dynamicDowncast<JSFunction>(ctorObject))
                                constructorFunctionName = constructorFunction->calculatedDisplayName(vm);
                            else if (InternalFunction* constructorFunction = dynamicDowncast<InternalFunction>(ctorObject))
                                constructorFunctionName = constructorFunction->calculatedDisplayName(vm);
                            else if (String name = isCompiledCalleeType(ctorObject->type()) ? getCalculatedDisplayName(vm, ctorObject) : String(); !name.isEmpty())
                                constructorFunctionName = name;
                        }
                    }
                }
            }
        }
    }

    EXCEPTION_ASSERT(!scope.exception() || constructorFunctionName.isNull());
    if (scope.exception()) [[unlikely]]
        scope.clearException();

    if (constructorFunctionName.isNull() || constructorFunctionName == "Object"_s) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        if (object->getPropertySlot(globalObject, vm.propertyNames->toStringTagSymbol, slot)) {
            EXCEPTION_ASSERT(!scope.exception());
            if (slot.isValue()) {
                JSValue value = slot.getValue(globalObject, vm.propertyNames->toStringTagSymbol);
                if (value.isString()) {
                    auto tag = asString(value)->value(globalObject);
                    if (scope.exception()) [[unlikely]]
                        scope.clearException();
                    return tag;
                }
            }
        }

        if (scope.exception()) [[unlikely]]
            scope.clearException();

        String classInfoName = object->classInfo()->className;
        if (!classInfoName.isNull())
            return classInfoName;

        if (constructorFunctionName.isNull())
            return "Object"_s;
    }

    return constructorFunctionName;
}

bool JSObject::getOwnPropertySlotByIndex(JSObject* thisObject, JSGlobalObject* globalObject, unsigned i, PropertySlot& slot)
{
    VM& vm = globalObject->vm();

    // NB. The fact that we're directly consulting our indexed storage implies that it is not
    // legal for anyone to override getOwnPropertySlot() without also overriding
    // getOwnPropertySlotByIndex().
    
    if (i > MAX_ARRAY_INDEX)
        return thisObject->methodTable()->getOwnPropertySlot(thisObject, globalObject, Identifier::from(vm, i), slot);
    
    switch (thisObject->indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        break;
        
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return false;
        
        JSValue value = butterfly->contiguous().at(thisObject, i).get();
        if (value) {
            slot.setValue(thisObject, isCopyOnWrite(thisObject->indexingMode()) && thisObject->hasFrozenElementsInPlace() ? PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete : static_cast<unsigned>(PropertyAttribute::None), value);
            return true;
        }
        
        return false;
    }
        
    case ALL_DOUBLE_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return false;
        
        double value = butterfly->contiguousDouble().at(thisObject, i);
        if (value == value) {
            slot.setValue(thisObject, isCopyOnWrite(thisObject->indexingMode()) && thisObject->hasFrozenElementsInPlace() ? PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete : static_cast<unsigned>(PropertyAttribute::None), JSValue(JSValue::EncodeAsDouble, value));
            return true;
        }
        
        return false;
    }
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = thisObject->butterfly()->arrayStorage();
        if (i >= storage->length())
            return false;
        
        if (i < storage->vectorLength()) {
            JSValue value = storage->m_vector[i].get();
            if (value) {
                slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), value);
                return true;
            }
        } else if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
            SparseArrayValueMap::iterator it = map->find(i);
            if (it != map->notFound()) {
                it->get(thisObject, slot);
                return true;
            }
        }
        break;
    }
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    
    return false;
}

#if ASSERT_ENABLED
// These needs to be unique (not inlined) for ASSERT_ENABLED builds to enable
// Structure::validateFlags() to do checks using function pointer comparisons.

bool JSObject::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    return getOwnPropertySlotImpl(object, globalObject, propertyName, slot);
}
#endif // ASSERT_ENABLED

// https://tc39.github.io/ecma262/#sec-ordinaryset
bool ordinarySetSlow(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value, JSValue receiver, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
#if ENABLE(AOT)
    // The receiver is a proxy of the object that hands definitions on to it. Where an element can be set quickly nothing above the object takes the store, so the store ends in the object as it does without the proxy.
    if (std::optional<uint32_t> index = parseIndex(propertyName); index && !object->structure()->hasImmutableIndexedProperties()) {
        JSObject* definedOn = AOT::RuntimeHooks::targetThatProxyDefinesOn(vm, receiver);
        RETURN_IF_EXCEPTION(scope, false);
        if (definedOn == object && object->trySetIndexQuickly(vm, *index, value))
            return true;
    }
#endif
    PropertyDescriptor ownDescriptor;
    if (object->type() != ProxyObjectType) {
        object->getOwnPropertyDescriptor(globalObject, propertyName, ownDescriptor);
        RETURN_IF_EXCEPTION(scope, false);
    }
    RELEASE_AND_RETURN(scope, ordinarySetWithOwnDescriptor(globalObject, object, propertyName, value, receiver, WTF::move(ownDescriptor), shouldThrow));
}

// https://tc39.es/ecma262/multipage/ordinary-and-exotic-objects-behaviours.html#sec-ordinarysetwithowndescriptor
bool ordinarySetWithOwnDescriptor(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value, JSValue receiver, PropertyDescriptor&& ownDescriptor, bool shouldThrow)
{
    // If we find the receiver is not the same to the object, we fall to this slow path.
    // Currently, there are 3 candidates.
    // 1. Reflect.set can alter the receiver with an arbitrary value.
    // 2. Window Proxy.
    // 3. ES6 Proxy.

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* current = object;
    while (true) {
        if (current->type() == ProxyObjectType) {
            auto* proxy = uncheckedDowncast<ProxyObject>(current);
            PutPropertySlot slot(receiver, shouldThrow);
            RELEASE_AND_RETURN(scope, proxy->ProxyObject::put(proxy, globalObject, propertyName, value, slot));
        }

        if (current != object && isTypedArrayType(current->type())) {
            PutPropertySlot slot(receiver, shouldThrow);
            RELEASE_AND_RETURN(scope, current->methodTable()->put(current, globalObject, propertyName, value, slot));
        }

        // 9.1.9.1-2 Let ownDesc be ? O.[[GetOwnProperty]](P).
        bool ownDescriptorFound;
        if (current == object)
            ownDescriptorFound = !ownDescriptor.isEmpty();
        else {
            ownDescriptorFound = current->getOwnPropertyDescriptor(globalObject, propertyName, ownDescriptor);
            RETURN_IF_EXCEPTION(scope, false);
        }

        if (!ownDescriptorFound) {
            // 9.1.9.1-3-a Let parent be ? O.[[GetPrototypeOf]]().
            JSValue prototype = current->getPrototype(globalObject);
            RETURN_IF_EXCEPTION(scope, false);

            // 9.1.9.1-3-b If parent is not null, then
            if (!prototype.isNull()) {
                // 9.1.9.1-3-b-i Return ? parent.[[Set]](P, V, Receiver).
                current = asObject(prototype);
                continue;
            }
            // 9.1.9.1-3-c-i Let ownDesc be the PropertyDescriptor{[[Value]]: undefined, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true}.
            ownDescriptor = PropertyDescriptor(jsUndefined(), static_cast<unsigned>(PropertyAttribute::None));
        }
        break;
    }

    // 9.1.9.1-4 If IsDataDescriptor(ownDesc) is true, then
    if (ownDescriptor.isDataDescriptor()) {
        // 9.1.9.1-4-a If ownDesc.[[Writable]] is false, return false.
        if (!ownDescriptor.writable())
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

        // 9.1.9.1-4-b If Type(Receiver) is not Object, return false.
        if (!receiver.isObject())
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

        // In OrdinarySet, the receiver may not be the same to the object.
        // So, we perform [[GetOwnProperty]] onto the receiver while we already perform [[GetOwnProperty]] onto the object.

        // 9.1.9.1-4-c Let existingDescriptor be ? Receiver.[[GetOwnProperty]](P).
        JSObject* receiverObject = asObject(receiver);
        PropertyDescriptor existingDescriptor;
        bool existingDescriptorFound = receiverObject->getOwnPropertyDescriptor(globalObject, propertyName, existingDescriptor);
        RETURN_IF_EXCEPTION(scope, false);

        // 9.1.9.1-4-d If existingDescriptor is not undefined, then
        if (existingDescriptorFound) {
            // 9.1.9.1-4-d-i If IsAccessorDescriptor(existingDescriptor) is true, return false.
            if (existingDescriptor.isAccessorDescriptor())
                return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

            // 9.1.9.1-4-d-ii If existingDescriptor.[[Writable]] is false, return false.
            if (!existingDescriptor.writable())
                return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

            // 9.1.9.1-4-d-iii Let valueDesc be the PropertyDescriptor{[[Value]]: V}.
            PropertyDescriptor valueDescriptor;
            valueDescriptor.setValue(value);

            // 9.1.9.1-4-d-iv Return ? Receiver.[[DefineOwnProperty]](P, valueDesc).
            RELEASE_AND_RETURN(scope, receiverObject->methodTable()->defineOwnProperty(receiverObject, globalObject, propertyName, valueDescriptor, shouldThrow));
        }

        // 9.1.9.1-4-e Else Receiver does not currently have a property P,
        // 9.1.9.1-4-e-i Return ? CreateDataProperty(Receiver, P, V).
        RELEASE_AND_RETURN(scope, receiverObject->methodTable()->defineOwnProperty(receiverObject, globalObject, propertyName, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), shouldThrow));
    }

    // 9.1.9.1-5 Assert: IsAccessorDescriptor(ownDesc) is true.
    ASSERT(ownDescriptor.isAccessorDescriptor());

    // 9.1.9.1-6 Let setter be ownDesc.[[Set]].
    // 9.1.9.1-7 If setter is undefined, return false.
    JSValue setter = ownDescriptor.setter();
    if (!setter.isObject())
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

    // 9.1.9.1-8 Perform ? Call(setter, Receiver, << V >>).
    JSObject* setterObject = asObject(setter);
    auto args = WTF::toArray<EncodedJSValue>({
        JSValue::encode(value),
    });

    auto callData = JSC::getCallData(setterObject);
    scope.release();
    call(globalObject, setterObject, callData, receiver, ArgList { args.data(), args.size() });

    // 9.1.9.1-9 Return true.
    return true;
}

bool setterThatIgnoresPrototypeProperties(JSGlobalObject* globalObject, JSValue thisValue, JSObject* homeObject, PropertyName propertyName, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!thisValue.isObject())
        return throwTypeError(globalObject, scope, "SetterThatIgnoresPrototypeProperties expected |this| to be an object."_s);

    JSObject* thisObject = asObject(thisValue);
    RETURN_IF_EXCEPTION(scope, { });

    if (thisObject == homeObject)
        return throwTypeError(globalObject, scope, "SetterThatIgnoresPrototypeProperties was called on a home object."_s);

    bool hasProperty = thisObject->hasOwnProperty(globalObject, propertyName);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();

    if (hasProperty) {
        PutPropertySlot slot(thisObject, shouldThrow);
        return thisObject->methodTable()->put(thisObject, globalObject, propertyName, value, slot);
    }

    return thisObject->createDataProperty(globalObject, propertyName, value, shouldThrow);
}

// https://tc39.es/ecma262/#sec-ordinaryset
bool JSObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    return putInlineForJSObject(cell, globalObject, propertyName, value, slot);
}

// As a definition compares the two: an equal string in another cell and a number in another form are the value that is held.
static bool holdsInWritableProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value)
{
    unsigned attributes;
    PropertyOffset offset = object->structure()->get(globalObject->vm(), propertyName, attributes);
    if (!isValidOffset(offset) || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)
        return false;
    return sameValue(globalObject, object->getDirect(offset), value);
}

// `object.__proto__ = value` where the put finds the accessor of a locked Object.prototype, which is the original one. Its setter leaves the object as it is
// where the value is neither an object nor null, or is what lies above the object already.
static bool changesNothingThroughSetterOfPrototype(VM& vm, JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value)
{
    if (propertyName.uid() != vm.propertyNames->underscoreProto.impl())
        return false;
    if ((value.isObject() || value.isNull()) && object->getPrototypeDirect() != value)
        return false;
    JSObject* holderOfAccessor = globalObject->objectPrototype();
    if (!holderOfAccessor->structure()->hasImmutableProperties())
        return false;
    for (JSObject* current = object; current != holderOfAccessor;) {
        Structure* structure = current->structure();
        unsigned attributes;
        // An array has a put of its own for `length` alone.
        if ((current != object && structure->typeInfo().overridesPut() && !current->inherits<JSArray>()) || structure->typeInfo().overridesGetPrototype() || structure->hasNonReifiedStaticProperties() || isValidOffset(structure->get(vm, propertyName, attributes)))
            return false;
        JSValue above = current->getPrototypeDirect();
        if (!above.isObject())
            return false;
        current = asObject(above);
    }
    return true;
}

bool JSObject::putInlineSlow(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    ASSERT(!parseIndex(propertyName));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (structure()->hasFrozenElementsInPlace()) [[unlikely]]
        moveFrozenElementsToSparseMap(vm);

    // Every put this object is the receiver of is refused: data properties, setters and custom setters alike. It comes here because
    // canPerformFastPutInlineExcludingProto() says no for such a receiver. A put of the value the property holds changes nothing and
    // succeeds, like a definition that changes nothing: `Object.assign = Object.assign || polyfill`.
    if (structure()->isImmutableProperty(propertyName.uid()) && !isThisValueAltered(slot, this)) [[unlikely]] {
        bool holdsValue = holdsInWritableProperty(globalObject, this, propertyName, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (holdsValue || changesNothingThroughSetterOfPrototype(vm, globalObject, this, propertyName, value))
            return true;
        RefusalIs refusalIs = slot.context() == PutPropertySlot::ReflectSet || isStoreRefusedByLanguage(globalObject, this, propertyName) ? RefusalIs::Answered : RefusalIs::ToldToOperation;
        return refuseChangeOfImmutableProperty(globalObject, scope, slot.isStrictMode(), refusalIs, this, propertyName, ReadonlyPropertyWriteError);
    }

    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return false;
    }

    JSObject* obj = this;
    for (;;) {
        Structure* structure = obj->structure();
        if (obj != this && structure->typeInfo().overridesPut())
            RELEASE_AND_RETURN(scope, obj->methodTable()->put(obj, globalObject, propertyName, value, slot));

        bool hasProperty = false;
        unsigned attributes;
        PutValueFunc customSetter = nullptr;
        PropertyOffset offset = structure->get(vm, propertyName, attributes);
        if (isValidOffset(offset)) {
            hasProperty = true;
            if (attributes & PropertyAttribute::CustomAccessorOrValue)
                customSetter = uncheckedDowncast<CustomGetterSetter>(obj->getDirect(offset))->setter();
        } else if (structure->hasNonReifiedStaticProperties()) {
            if (auto entry = structure->findPropertyHashEntry(propertyName)) {
                hasProperty = true;
                attributes = entry->value->attributes();

                // FIXME: Remove this after we stop defaulting to CustomValue in static hash tables.
                if (!(attributes & (PropertyAttribute::CustomAccessor | PropertyAttribute::BuiltinOrFunctionOrAccessorOrLazyPropertyOrConstant)))
                    attributes |= PropertyAttribute::CustomValue;

                if (attributes & PropertyAttribute::CustomAccessorOrValue)
                    customSetter = functionUnlessRemoved(entry->value->propertyPutter());
            }
        }

        if (hasProperty) {
            if (attributes & PropertyAttribute::ReadOnly)
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            if (attributes & PropertyAttribute::Accessor) {
                ASSERT(isValidOffset(offset));
                // We need to make sure that we decide to cache this property before we potentially execute aribitrary JS.
                if (!this->structure()->isUncacheableDictionary())
                    slot.setCacheableSetter(obj, offset);
                RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(obj->getDirect(offset))->callSetter(globalObject, slot.thisValue(), value, slot.isStrictMode()));
            }
            if (attributes & PropertyAttribute::CustomAccessor) {
                // FIXME: Remove this after WebIDL generator is fixed to set ReadOnly for [RuntimeConditionallyReadWrite] attributes.
                if (!customSetter)
                    return false;
                ASSERT(customSetter);
                // FIXME: We should only be caching these if we're not an uncacheable dictionary:
                // https://bugs.webkit.org/show_bug.cgi?id=215347
                slot.setCustomAccessor(obj, customSetter, offset);
                scope.release();
                customSetter(obj->realm(), JSValue::encode(slot.thisValue()), JSValue::encode(value), propertyName);
                return true;
            }
            // (The two shortcuts below put on the receiver without asking. A receiver with immutable properties goes on to
            // definePropertyOnReceiver(), where its [[DefineOwnProperty]] decides.)
            if (attributes & PropertyAttribute::CustomValue) {
                if (!isThisValueAltered(slot, obj) && !obj->structure()->isImmutableProperty(propertyName.uid())) {
                    if (customSetter) {
                        // FIXME: We should only be caching these if we're not an uncacheable dictionary:
                        // https://bugs.webkit.org/show_bug.cgi?id=215347
                        slot.setCustomValue(obj, customSetter, offset);
                        RELEASE_AND_RETURN(scope, customSetter(obj->realm(), JSValue::encode(obj), JSValue::encode(value), propertyName));
                    }
                    // Avoid PutModePut because it fails for non-extensible structures.
                    obj->putDirect(vm, propertyName, value, attributesForStructure(attributes) & ~PropertyAttribute::CustomValue, slot);
                    return true;
                }
            }
            if (attributes & PropertyAttribute::BuiltinOrFunctionOrLazyProperty) {
                if (!isThisValueAltered(slot, obj) && !obj->structure()->isImmutableProperty(propertyName.uid())) {
                    // Avoid PutModePut because it fails for non-extensible structures.
                    obj->putDirect(vm, propertyName, value, attributesForStructure(attributes), slot);
                    return true;
                }
            }
            // If there's an existing writable property on the base object, or on one of its 
            // prototypes, we should attempt to store the property on the receiver.
            break;
        }

        JSValue prototype = obj->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (prototype.isNull())
            break;
        obj = asObject(prototype);
    }

    scope.release();
    if (isThisValueAltered(slot, this)) [[unlikely]]
        return definePropertyOnReceiver(globalObject, propertyName, value, slot);
    return putInlineFast(globalObject, propertyName, value, slot);
}

bool JSObject::mightBeSpecialProperty(VM& vm, JSType type, UniquedStringImpl* uid)
{
    switch (type) {
    case ArrayType:
    case DerivedArrayType:
        return uid == vm.propertyNames->length.impl();
    case JSFunctionType:
        return uid == vm.propertyNames->length.impl() || uid == vm.propertyNames->name.impl() || uid == vm.propertyNames->prototype.impl();
    default:
        return true;
    }
}

static NEVER_INLINE bool definePropertyOnReceiverSlow(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, JSObject* receiver, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PropertySlot slot(receiver, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = receiver->methodTable()->getOwnPropertySlot(receiver, globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, false);

    if (hasProperty) {
        // FIXME: For an accessor with setter, the error message is misleading.
        if (slot.attributes() & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor)
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

        if (slot.attributes() & PropertyAttribute::CustomValue) {
            // (Not for a receiver with immutable properties: a native setter does not ask, and [[DefineOwnProperty]] below does.)
            PutValueFunc customSetter = slot.customSetter();
            if (customSetter && !receiver->structure()->isImmutableProperty(propertyName.uid()))
                RELEASE_AND_RETURN(scope, customSetter(receiver->realm(), JSValue::encode(receiver), JSValue::encode(value), propertyName));
        }

        PropertyDescriptor descriptor;
        descriptor.setValue(value);
        RELEASE_AND_RETURN(scope, receiver->methodTable()->defineOwnProperty(receiver, globalObject, propertyName, descriptor, shouldThrow));
    }

    RELEASE_AND_RETURN(scope, receiver->createDataProperty(globalObject, propertyName, value, shouldThrow));
}

// https://tc39.es/ecma262/#sec-ordinaryset (step 3)
bool JSObject::definePropertyOnReceiver(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    ASSERT(!parseIndex(propertyName));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* receiver = slot.thisValue().getObject();
    // FIXME: For a failure due to primitive receiver, the error message is misleading.
    if (!receiver)
        return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
    if (receiver->type() == GlobalProxyType)
        receiver = uncheckedDowncast<JSGlobalProxy>(receiver)->target();
#if ENABLE(AOT)
    if (JSObject* definedOn = AOT::RuntimeHooks::targetThatProxyDefinesOn(vm, receiver); definedOn && definedOn->type() != ProxyObjectType && definedOn->type() != CompiledObjectType)
        receiver = definedOn;
    RETURN_IF_EXCEPTION(scope, false);
#endif
    scope.release();
    // The steps as the specification has them, in which the receiver's [[DefineOwnProperty]] decides: the shortcuts below put.
    if (receiver->structure()->isImmutableProperty(propertyName.uid())) [[unlikely]] {
        // An owner whose refusal always throws is heard here as where the store begins at the receiver.
        bool isTold = slot.isStrictMode()
            || (slot.context() != PutPropertySlot::ReflectSet && refusalOfOwner(globalObject, receiver, propertyName.uid(), false).alwaysThrows && !isStoreRefusedByLanguage(globalObject, receiver, propertyName));
        return definePropertyOnReceiverSlow(globalObject, propertyName, value, receiver, isTold);
    }

    if (slot.isTaintedByOpaqueObject() || receiver->methodTable()->defineOwnProperty != JSObject::defineOwnProperty) {
        if (mightBeSpecialProperty(vm, receiver->type(), propertyName.uid()))
            return definePropertyOnReceiverSlow(globalObject, propertyName, value, receiver, slot.isStrictMode());
    }

    if (receiver->structure()->hasAnyKindOfGetterSetterProperties()) {
        unsigned attributes;
        if (receiver->getDirectOffset(vm, propertyName, attributes) != invalidOffset && (attributes & PropertyAttribute::CustomValue))
            return definePropertyOnReceiverSlow(globalObject, propertyName, value, receiver, slot.isStrictMode());
    }

    if (receiver->hasNonReifiedStaticProperties()) [[unlikely]]
        return receiver->putInlineFastReplacingStaticPropertyIfNeeded(globalObject, propertyName, value, slot);
    return receiver->putInlineFast(globalObject, propertyName, value, slot);
}

bool JSObject::putInlineFastReplacingStaticPropertyIfNeeded(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    ASSERT(!parseIndex(propertyName));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Structure* structure = this->structure();
    ASSERT(structure->hasNonReifiedStaticProperties());
    if (!isValidOffset(structure->get(vm, propertyName))) {
        if (auto entry = structure->findPropertyHashEntry(propertyName)) {
            if (entry->value->attributes() & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor) {
                // FIXME: For an accessor with setter, the error message is misleading.
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            }
            if (entry->value->attributes() & PropertyAttribute::CustomValue) {
                PutValueFunc customSetter = functionUnlessRemoved(entry->value->propertyPutter());
                if (customSetter)
                    RELEASE_AND_RETURN(scope, customSetter(structure->realm(), JSValue::encode(this), JSValue::encode(value), propertyName));
            }
            // Avoid PutModePut because it fails for non-extensible structures.
            putDirect(vm, propertyName, value, attributesForStructure(entry->value->attributes()) & ~PropertyAttribute::CustomValue, slot);
            return true;
        }
    }

    RELEASE_AND_RETURN(scope, putInlineFast(globalObject, propertyName, value, slot));
}

bool JSObject::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned propertyName, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);

    if (thisObject->structure()->hasImmutableIndexedProperties()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        // Nothing says here who asks: Reflect.set() answers for such an object by itself.
        Identifier key = Identifier::from(vm, propertyName);
        RefusalOfOwner refusal = refusalOfOwner(globalObject, thisObject, key.impl(), false);
        if (refusal.alwaysThrows && !refusal.message.isNull())
            return typeError(globalObject, scope, true, refusal.message);
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    }

    if (propertyName > MAX_ARRAY_INDEX) {
        PutPropertySlot slot(cell, shouldThrow);
        return thisObject->methodTable()->put(thisObject, globalObject, Identifier::from(vm, propertyName), value, slot);
    }

    thisObject->ensureWritable(vm);

    switch (thisObject->indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        break;
        
    case ALL_UNDECIDED_INDEXING_TYPES: {
        thisObject->convertUndecidedForValue(vm, value);
        // Reloop.
        return putByIndex(cell, globalObject, propertyName, value, shouldThrow);
    }
        
    case ALL_INT32_INDEXING_TYPES: {
        if (!value.isInt32()) {
            thisObject->convertInt32ForValue(vm, value);
            return putByIndex(cell, globalObject, propertyName, value, shouldThrow);
        }
        [[fallthrough]];
    }
        
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (propertyName >= butterfly->vectorLength())
            break;
        butterfly->contiguous().at(thisObject, propertyName).setWithoutWriteBarrier(value);
        if (propertyName >= butterfly->publicLength())
            butterfly->setPublicLength(propertyName + 1);
        vm.writeBarrier(thisObject, value);
        return true;
    }
        
    case ALL_DOUBLE_INDEXING_TYPES: {
        if (!value.isNumber()) {
            thisObject->convertDoubleToContiguous(vm);
            // Reloop.
            return putByIndex(cell, globalObject, propertyName, value, shouldThrow);
        }

        double valueAsDouble = value.asNumber();
        if (valueAsDouble != valueAsDouble) {
            thisObject->convertDoubleToContiguous(vm);
            // Reloop.
            return putByIndex(cell, globalObject, propertyName, value, shouldThrow);
        }
        Butterfly* butterfly = thisObject->butterfly();
        if (propertyName >= butterfly->vectorLength())
            break;
        butterfly->contiguousDouble().at(thisObject, propertyName) = valueAsDouble;
        if (propertyName >= butterfly->publicLength())
            butterfly->setPublicLength(propertyName + 1);
        return true;
    }
        
    case NonArrayWithArrayStorage:
    case ArrayWithArrayStorage: {
        ArrayStorage* storage = thisObject->butterfly()->arrayStorage();
        
        if (propertyName >= storage->vectorLength())
            break;
        
        WriteBarrier<Unknown>& valueSlot = storage->m_vector[propertyName];
        unsigned length = storage->length();
        
        // Update length & m_numValuesInVector as necessary.
        if (propertyName >= length) {
            length = propertyName + 1;
            storage->setLength(length);
            ++storage->m_numValuesInVector;
        } else if (!valueSlot)
            ++storage->m_numValuesInVector;
        
        valueSlot.set(vm, thisObject, value);
        return true;
    }
        
    case NonArrayWithSlowPutArrayStorage:
    case ArrayWithSlowPutArrayStorage: {
        ArrayStorage* storage = thisObject->butterfly()->arrayStorage();
        
        if (propertyName >= storage->vectorLength())
            break;
        
        WriteBarrier<Unknown>& valueSlot = storage->m_vector[propertyName];
        unsigned length = storage->length();

        auto scope = DECLARE_THROW_SCOPE(vm);
        
        // Update length & m_numValuesInVector as necessary.
        if (propertyName >= length) {
            bool putResult = false;
            bool result = thisObject->attemptToInterceptPutByIndexOnHole(globalObject, propertyName, value, shouldThrow, putResult);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return putResult;
            length = propertyName + 1;
            storage->setLength(length);
            ++storage->m_numValuesInVector;
        } else if (!valueSlot) {
            bool putResult = false;
            bool result = thisObject->attemptToInterceptPutByIndexOnHole(globalObject, propertyName, value, shouldThrow, putResult);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return putResult;
            ++storage->m_numValuesInVector;
        }
        
        valueSlot.set(vm, thisObject, value);
        return true;
    }
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    return thisObject->putByIndexBeyondVectorLength(globalObject, propertyName, value, shouldThrow);
}

ArrayStorage* JSObject::enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(VM& vm, ArrayStorage* storage)
{
    SparseArrayValueMap* map = storage->m_sparseMap.get();

    if (!map)
        map = allocateSparseIndexMap(vm);

    if (map->sparseMode())
        return storage;

    map->setSparseMode();

    unsigned usedVectorLength = std::min(storage->length(), storage->vectorLength());
    for (unsigned i = 0; i < usedVectorLength; ++i) {
        JSValue value = storage->m_vector[i].get();
        // This will always be a new entry in the map, so no need to check we can write,
        // and attributes are default so no need to set them.
        if (value)
            map->add(this, i).iterator->forceSet(vm, map, value, 0);
    }

    DeferGC deferGC(vm);
    Butterfly* newButterfly = storage->butterfly()->resizeArray(vm, this, structure(), 0, ArrayStorage::sizeFor(0));
    RELEASE_ASSERT(newButterfly);
    newButterfly->arrayStorage()->m_indexBias = 0;
    newButterfly->arrayStorage()->setVectorLength(0);
    newButterfly->arrayStorage()->m_sparseMap.set(vm, this, map);
    setButterfly(vm, newButterfly);
    
    return newButterfly->arrayStorage();
}

void JSObject::enterDictionaryIndexingMode(VM& vm)
{
    switch (indexingType()) {
    case NonArray:
        // No indexed properties to convert. Once the caller makes the structure
        // non-extensible, indexingShouldBeSparse() lazily handles later indexed
        // writes; staying blank also keeps for-in enumerator caching usable.
        // JSArray code paths (e.g. setLengthWritable) assume this method
        // allocated ArrayStorage, so do not skip for JSArray subclasses that
        // use NonArray indexing (e.g. $vm RuntimeArray with DerivedArrayType).
        if (!inherits<JSArray>()) [[likely]]
            return;
        [[fallthrough]];
    case ArrayClass:
    case ALL_UNDECIDED_INDEXING_TYPES:
    case ALL_INT32_INDEXING_TYPES:
    case ALL_DOUBLE_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        // NOTE: this is horribly inefficient, as it will perform two conversions. We could optimize
        // this case if we ever cared. Note that ensureArrayStorage() can return null if the object
        // doesn't support traditional indexed properties. At the time of writing, this just affects
        // typed arrays.
        if (ArrayStorage* storage = ensureArrayStorageSlow(vm))
            enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, storage);
        break;
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, this->butterfly()->arrayStorage());
        break;
        
    default:
        break;
    }
}

void JSObject::notifyPresenceOfIndexedAccessors(VM& vm)
{
    if (isGlobalObject()) [[unlikely]] {
        uncheckedDowncast<JSGlobalObject>(this)->globalThis()->notifyPresenceOfIndexedAccessors(vm);
        return;
    }

    if (mayInterceptIndexedAccesses())
        return;

    auto* globalObject = realmMayBeNull();
    if (!globalObject)
        return;

    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AddIndexedAccessors, &deferred));
    }

    if (!mayBePrototype())
        return;

    globalObject->haveABadTime(vm, this);
}

Butterfly* JSObject::createInitialIndexedStorage(VM& vm, unsigned length)
{
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    IndexingType oldType = indexingType();
    ASSERT_UNUSED(oldType, !hasIndexedProperties(oldType));
    ASSERT(!needsSlowPutIndexing());
    ASSERT(!indexingShouldBeSparse());
    Structure* structure = this->structure();
    unsigned propertyCapacity = structure->outOfLineCapacity();
    unsigned vectorLength = Butterfly::optimalContiguousVectorLength(propertyCapacity, length);
    Butterfly* newButterfly = Butterfly::createOrGrowArrayRight(
        this->butterfly(), vm, this, structure, propertyCapacity, false, 0,
        sizeof(EncodedJSValue) * vectorLength);
    newButterfly->setPublicLength(length);
    newButterfly->setVectorLength(vectorLength);
    return newButterfly;
}

Butterfly* JSObject::createInitialUndecided(VM& vm, unsigned length)
{
    DeferGC deferGC(vm);
    Butterfly* newButterfly = createInitialIndexedStorage(vm, length);
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateUndecided, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return newButterfly;
}

ContiguousJSValues JSObject::createInitialInt32(VM& vm, unsigned length)
{
    DeferGC deferGC(vm);
    Butterfly* newButterfly = createInitialIndexedStorage(vm, length);
    for (unsigned i = newButterfly->vectorLength(); i--;)
        newButterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateInt32, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return newButterfly->contiguousInt32();
}

ContiguousDoubles JSObject::createInitialDouble(VM& vm, unsigned length)
{
    DeferGC deferGC(vm);
    Butterfly* newButterfly = createInitialIndexedStorage(vm, length);
    for (unsigned i = newButterfly->vectorLength(); i--;)
        newButterfly->contiguousDouble().at(this, i) = PNaN;
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateDouble, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return newButterfly->contiguousDouble();
}

ContiguousJSValues JSObject::createInitialContiguous(VM& vm, unsigned length)
{
    DeferGC deferGC(vm);
    Butterfly* newButterfly = createInitialIndexedStorage(vm, length);
    for (unsigned i = newButterfly->vectorLength(); i--;)
        newButterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return newButterfly->contiguous();
}

static Butterfly* createArrayStorageButterflyImpl(VM& vm, JSObject* intendedOwner, Structure* structure, unsigned length, unsigned vectorLength, Butterfly* oldButterfly, AllocationFailureMode mode)
{
    Butterfly* newButterfly = Butterfly::createOrGrowArrayRight(
        oldButterfly, vm, intendedOwner, structure, structure->outOfLineCapacity(), false, 0,
        ArrayStorage::sizeFor(vectorLength));
    if (!newButterfly) [[unlikely]] {
        RELEASE_ASSERT_RESOURCE_AVAILABLE(mode != AllocationFailureMode::Assert, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
        return nullptr;
    }

    ArrayStorage* result = newButterfly->arrayStorage();
    result->setLength(length);
    result->setVectorLength(vectorLength);
    result->m_sparseMap.clear();
    result->m_numValuesInVector = 0;
    result->m_indexBias = 0;
    for (size_t i = vectorLength; i--;)
        result->m_vector[i].setWithoutWriteBarrier(JSValue());

    return newButterfly;
}

Butterfly* JSObject::createArrayStorageButterfly(VM& vm, JSObject* intendedOwner, Structure* structure, unsigned length, unsigned vectorLength, Butterfly* oldButterfly)
{
    return createArrayStorageButterflyImpl(vm, intendedOwner, structure, length, vectorLength, oldButterfly, AllocationFailureMode::Assert);
}

Butterfly* JSObject::tryCreateArrayStorageButterfly(VM& vm, JSObject* intendedOwner, Structure* structure, unsigned length, unsigned vectorLength, Butterfly* oldButterfly)
{
    return createArrayStorageButterflyImpl(vm, intendedOwner, structure, length, vectorLength, oldButterfly, AllocationFailureMode::ReturnNull);
}


ArrayStorage* JSObject::createArrayStorage(VM& vm, unsigned length, unsigned vectorLength)
{
    DeferGC deferGC(vm);
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    IndexingType oldType = indexingType();
    ASSERT_UNUSED(oldType, !hasIndexedProperties(oldType));

    Butterfly* newButterfly = createArrayStorageButterfly(vm, this, oldStructure, length, vectorLength, this->butterfly());
    ArrayStorage* result = newButterfly->arrayStorage();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, suggestedArrayStorageTransition(), &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return result;
}

ArrayStorage* JSObject::createInitialArrayStorage(VM& vm)
{
    return createArrayStorage(
        vm, 0, ArrayStorage::optimalVectorLength(0, structure()->outOfLineCapacity(), 0));
}

ContiguousJSValues JSObject::convertUndecidedToInt32(VM& vm)
{
    ASSERT(hasUndecided(indexingType()));

    Butterfly* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;)
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());

    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateInt32, &deferred));
    }
    return this->butterfly()->contiguousInt32();
}

ContiguousDoubles JSObject::convertUndecidedToDouble(VM& vm)
{
    ASSERT(Options::allowDoubleShape());
    ASSERT(hasUndecided(indexingType()));

    auto* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;)
        butterfly->contiguousDouble().at(this, i) = PNaN;
    
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateDouble, &deferred));
    }
    return this->butterfly()->contiguousDouble();
}

ContiguousJSValues JSObject::convertUndecidedToContiguous(VM& vm)
{
    ASSERT(hasUndecided(indexingType()));

    auto* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;)
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());

    WTF::storeStoreFence();
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred));
    }
    return this->butterfly()->contiguous();
}

ArrayStorage* JSObject::constructConvertedArrayStorageWithoutCopyingElements(VM& vm, unsigned neededLength)
{
    Structure* structure = this->structure();
    unsigned publicLength = this->butterfly()->publicLength();
    unsigned propertyCapacity = structure->outOfLineCapacity();

    Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, true, ArrayStorage::sizeFor(neededLength));

    // memcpy is fine since newButterfly is not tied to any object yet.
    memcpy(
        static_cast<JSValue*>(newButterfly->base(0, propertyCapacity)),
        static_cast<JSValue*>(this->butterfly()->base(0, propertyCapacity)),
        propertyCapacity * sizeof(EncodedJSValue));

    ArrayStorage* newStorage = newButterfly->arrayStorage();
    newStorage->setVectorLength(neededLength);
    newStorage->setLength(publicLength);
    newStorage->m_sparseMap.clear();
    newStorage->m_indexBias = 0;
    newStorage->m_numValuesInVector = 0;
    
    return newStorage;
}

ArrayStorage* JSObject::convertUndecidedToArrayStorage(VM& vm, TransitionKind transition)
{
    DeferGC deferGC(vm);
    ASSERT(hasUndecided(indexingType()));

    unsigned vectorLength = this->butterfly()->vectorLength();
    ArrayStorage* storage = constructConvertedArrayStorageWithoutCopyingElements(vm, vectorLength);
    
    for (unsigned i = vectorLength; i--;)
        storage->m_vector[i].setWithoutWriteBarrier(JSValue());
    
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, storage->butterfly());
        setStructure(vm, newStructure);
    }
    return storage;
}

ArrayStorage* JSObject::convertUndecidedToArrayStorage(VM& vm)
{
    return convertUndecidedToArrayStorage(vm, suggestedArrayStorageTransition());
}

ContiguousDoubles JSObject::convertInt32ToDouble(VM& vm)
{
    ASSERT(hasInt32(indexingType()));
    ASSERT(!isCopyOnWrite(indexingMode()));

    auto* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;) {
        WriteBarrier<Unknown>* current = &butterfly->contiguous().atUnsafe(i);
        double* currentAsDouble = std::bit_cast<double*>(current);
        JSValue v = current->get();
        // NOTE: Since this may be used during initialization, v could be garbage. If it's garbage,
        // that means it will be overwritten later.
        if (!v.isInt32()) {
            *currentAsDouble = PNaN;
            continue;
        }
        *currentAsDouble = v.asInt32();
    }
    
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateDouble, &deferred));
    }
    return this->butterfly()->contiguousDouble();
}

ContiguousJSValues JSObject::convertInt32ToContiguous(VM& vm)
{
    ASSERT(hasInt32(indexingType()));
    
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred));
    }
    return this->butterfly()->contiguous();
}

ArrayStorage* JSObject::convertInt32ToArrayStorage(VM& vm, TransitionKind transition)
{
    DeferGC deferGC(vm);
    ASSERT(hasInt32(indexingType()));

    unsigned vectorLength = this->butterfly()->vectorLength();
    ArrayStorage* newStorage = constructConvertedArrayStorageWithoutCopyingElements(vm, vectorLength);
    auto* butterfly = this->butterfly();
    for (unsigned i = 0; i < vectorLength; i++) {
        JSValue v = butterfly->contiguous().at(this, i).get();
        newStorage->m_vector[i].setWithoutWriteBarrier(v);
        if (v)
            newStorage->m_numValuesInVector++;
    }
    
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newStorage->butterfly());
        setStructure(vm, newStructure);
    }
    return newStorage;
}

ArrayStorage* JSObject::convertInt32ToArrayStorage(VM& vm)
{
    return convertInt32ToArrayStorage(vm, suggestedArrayStorageTransition());
}

ContiguousJSValues JSObject::convertDoubleToContiguous(VM& vm)
{
    ASSERT(hasDouble(indexingType()));
    ASSERT(!isCopyOnWrite(indexingMode()));

    auto* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;) {
        double* current = &butterfly->contiguousDouble().atUnsafe(i);
        WriteBarrier<Unknown>* currentAsValue = std::bit_cast<WriteBarrier<Unknown>*>(current);
        double value = *current;
        if (value != value) {
            currentAsValue->clear();
            continue;
        }
        JSValue v = JSValue(JSValue::EncodeAsDouble, value);
        currentAsValue->setWithoutWriteBarrier(v);
    }
    
    WTF::storeStoreFence();
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred));
    }
    return this->butterfly()->contiguous();
}

ArrayStorage* JSObject::convertDoubleToArrayStorage(VM& vm, TransitionKind transition)
{
    DeferGC deferGC(vm);
    ASSERT(hasDouble(indexingType()));

    unsigned vectorLength = this->butterfly()->vectorLength();
    ArrayStorage* newStorage = constructConvertedArrayStorageWithoutCopyingElements(vm, vectorLength);
    auto* butterfly = this->butterfly();
    for (unsigned i = 0; i < vectorLength; i++) {
        double value = butterfly->contiguousDouble().at(this, i);
        if (value != value) {
            newStorage->m_vector[i].clear();
            continue;
        }
        newStorage->m_vector[i].setWithoutWriteBarrier(JSValue(JSValue::EncodeAsDouble, value));
        newStorage->m_numValuesInVector++;
    }
    
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newStorage->butterfly());
        setStructure(vm, newStructure);
    }
    return newStorage;
}

ArrayStorage* JSObject::convertDoubleToArrayStorage(VM& vm)
{
    return convertDoubleToArrayStorage(vm, suggestedArrayStorageTransition());
}

ArrayStorage* JSObject::convertContiguousToArrayStorage(VM& vm, TransitionKind transition)
{
    DeferGC deferGC(vm);
    ASSERT(hasContiguous(indexingType()));

    unsigned vectorLength = this->butterfly()->vectorLength();
    ArrayStorage* newStorage = constructConvertedArrayStorageWithoutCopyingElements(vm, vectorLength);
    auto* butterfly = this->butterfly();
    for (unsigned i = 0; i < vectorLength; i++) {
        JSValue v = butterfly->contiguous().at(this, i).get();
        newStorage->m_vector[i].setWithoutWriteBarrier(v);
        if (v)
            newStorage->m_numValuesInVector++;
    }

    // While we modify the butterfly of Contiguous Array, we do not take any cellLock here. This is because
    // (1) the old butterfly is not changed and (2) new butterfly is not changed after it is exposed to
    // the collector.
    // The mutator performs the following operations are sequentially executed by using storeStoreFence.
    //
    //     CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure
    //
    // Meanwhile the collector performs the following steps sequentially:
    //
    //     ReadStructureEarly ReadButterfly ReadStructureLate
    //
    // We list up all the patterns by writing a tiny script, and ensure all the cases are categorized into BEFORE, AFTER, and IGNORE.
    //
    // CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureEarly ReadButterfly ReadStructureLate: AFTER, trivially
    // CreateNewButterfly NukeStructure ChangeButterfly ReadStructureEarly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ChangeButterfly ReadStructureEarly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ChangeButterfly ReadStructureEarly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ChangeButterfly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ChangeButterfly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ChangeButterfly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ReadButterfly ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ReadButterfly ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ReadButterfly ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read early
    // CreateNewButterfly ReadStructureEarly NukeStructure ChangeButterfly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because early and late structures don't match
    // CreateNewButterfly ReadStructureEarly NukeStructure ChangeButterfly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // CreateNewButterfly ReadStructureEarly NukeStructure ChangeButterfly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly NukeStructure ReadButterfly ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // CreateNewButterfly ReadStructureEarly NukeStructure ReadButterfly ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly NukeStructure ReadButterfly ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly ReadButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // CreateNewButterfly ReadStructureEarly ReadButterfly NukeStructure ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly ReadButterfly NukeStructure ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly ReadButterfly ReadStructureLate NukeStructure ChangeButterfly PutNewStructure: BEFORE, trivially.
    // ReadStructureEarly CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly CreateNewButterfly NukeStructure ChangeButterfly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly CreateNewButterfly NukeStructure ChangeButterfly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly NukeStructure ReadButterfly ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly CreateNewButterfly NukeStructure ReadButterfly ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly NukeStructure ReadButterfly ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly ReadButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly CreateNewButterfly ReadButterfly NukeStructure ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly ReadButterfly NukeStructure ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly ReadButterfly ReadStructureLate NukeStructure ChangeButterfly PutNewStructure: BEFORE, CreateNewButterfly is not visible to collector.
    // ReadStructureEarly ReadButterfly CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly ReadButterfly CreateNewButterfly NukeStructure ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly ReadButterfly CreateNewButterfly NukeStructure ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly ReadButterfly CreateNewButterfly ReadStructureLate NukeStructure ChangeButterfly PutNewStructure: BEFORE, CreateNewButterfly is not visible to collector.
    // ReadStructureEarly ReadButterfly ReadStructureLate CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure: BEFORE, trivially.

    ASSERT(newStorage->butterfly() != butterfly);
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);

        // Ensure new Butterfly initialization is correctly done before exposing it to the concurrent threads.
        if (isX86() || vm.heap.mutatorShouldBeFenced())
            WTF::storeStoreFence();
        nukeStructureAndSetButterfly(vm, oldStructureID, newStorage->butterfly());
        setStructure(vm, newStructure);
    }
    
    return newStorage;
}

ArrayStorage* JSObject::convertContiguousToArrayStorage(VM& vm)
{
    return convertContiguousToArrayStorage(vm, suggestedArrayStorageTransition());
}

void JSObject::convertToIndexingTypeIfNeeded(VM& vm, IndexingType nextType)
{
    IndexingType currentType = indexingType();
    if (currentType == nextType)
        return;
    switch (currentType) {
    case ArrayWithUndecided: {
        switch (nextType) {
        case ArrayWithInt32:
            convertUndecidedToInt32(vm);
            break;
        case ArrayWithDouble:
            convertUndecidedToDouble(vm);
            break;
        case ArrayWithContiguous:
            convertUndecidedToContiguous(vm);
            break;
        case ArrayWithArrayStorage:
            convertUndecidedToArrayStorage(vm);
            break;
        }
        break;
    }
    case ArrayWithInt32: {
        switch (nextType) {
        case ArrayWithDouble:
            convertInt32ToDouble(vm);
            break;
        case ArrayWithContiguous:
            convertInt32ToContiguous(vm);
            break;
        case ArrayWithArrayStorage:
            convertInt32ToArrayStorage(vm);
            break;
        }
        break;
    }
    case ArrayWithDouble: {
        switch (nextType) {
        case ArrayWithContiguous:
            convertDoubleToContiguous(vm);
            break;
        case ArrayWithArrayStorage:
            convertDoubleToArrayStorage(vm);
            break;
        }
        break;
    }
    case ArrayWithContiguous: {
        switch (nextType) {
        case ArrayWithArrayStorage:
            convertContiguousToArrayStorage(vm);
            break;
        }
        break;
    }
    }
}

void JSObject::convertUndecidedForValue(VM& vm, JSValue value)
{
    IndexingType type = indexingTypeForValue(value);
    if (type == Int32Shape) {
        convertUndecidedToInt32(vm);
        return;
    }
    
    if (type == DoubleShape) {
        ASSERT(Options::allowDoubleShape());
        convertUndecidedToDouble(vm);
        return;
    }

    ASSERT(type == ContiguousShape);
    convertUndecidedToContiguous(vm);
}

void JSObject::createInitialForValueAndSet(VM& vm, unsigned index, JSValue value)
{
    if (value.isInt32()) {
        createInitialInt32(vm, index + 1).at(this, index).set(vm, this, value);
        return;
    }
    
    if (value.isDouble() && Options::allowDoubleShape()) {
        double doubleValue = value.asNumber();
        if (doubleValue == doubleValue) {
            createInitialDouble(vm, index + 1).at(this, index) = doubleValue;
            return;
        }
    }
    
    createInitialContiguous(vm, index + 1).at(this, index).set(vm, this, value);
}

void JSObject::convertInt32ForValue(VM& vm, JSValue value)
{
    ASSERT(!value.isInt32());
    
    if (value.isDouble() && !std::isnan(value.asDouble()) && Options::allowDoubleShape()) {
        convertInt32ToDouble(vm);
        return;
    }

    convertInt32ToContiguous(vm);
}

void JSObject::convertFromCopyOnWrite(VM& vm)
{
    ASSERT(isCopyOnWrite(indexingMode()));
    // Every caller that reaches this for an object with immutable properties is about to write its elements in place, except defineOwnIndexedProperty() where it
    // applies new attributes: it moves the elements on to a sparse map at once.
    RELEASE_ASSERT(!structure()->hasImmutableProperties() || vm.allowLazyMaterializationOfImmutablePropertiesCount);
    ASSERT(structure()->indexingMode() == indexingMode());

    const bool hasIndexingHeader = true;
    Butterfly* oldButterfly = this->butterfly();
    size_t propertyCapacity = 0;
    unsigned newVectorLength = Butterfly::optimalContiguousVectorLength(propertyCapacity, std::min<size_t>(nextLength(oldButterfly->vectorLength()), MAX_STORAGE_VECTOR_LENGTH));
    Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, hasIndexingHeader, newVectorLength * sizeof(JSValue));

    // memcpy is fine since newButterfly is not tied to any object yet.
    memcpy(newButterfly->propertyStorage(), oldButterfly->propertyStorage(), oldButterfly->vectorLength() * sizeof(JSValue) + sizeof(IndexingHeader));

    WTF::storeStoreFence();
    TransitionKind transition = ([&] () {
        switch (indexingType()) {
        case ArrayWithInt32:
            return TransitionKind::AllocateInt32;
        case ArrayWithDouble:
            return TransitionKind::AllocateDouble;
        case ArrayWithContiguous:
            return TransitionKind::AllocateContiguous;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return TransitionKind::AllocateContiguous;
        }
    })();
    StructureID oldStructureID = structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
}

void JSObject::setIndexQuicklyToUndecided(VM& vm, unsigned index, JSValue value)
{
    ASSERT(index < this->butterfly()->publicLength());
    ASSERT(index < this->butterfly()->vectorLength());
    convertUndecidedForValue(vm, value);
    setIndexQuickly(vm, index, value);
}

void JSObject::convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(VM& vm, unsigned index, JSValue value)
{
    ASSERT(!value.isInt32());
    convertInt32ForValue(vm, value);
    setIndexQuickly(vm, index, value);
}

void JSObject::convertDoubleToContiguousWhilePerformingSetIndex(VM& vm, unsigned index, JSValue value)
{
    ASSERT(!value.isNumber() || value.asNumber() != value.asNumber());
    convertDoubleToContiguous(vm);
    setIndexQuickly(vm, index, value);
}

ContiguousJSValues JSObject::tryMakeWritableInt32Slow(VM& vm)
{
    ASSERT(inherits(info()));

    if (isCopyOnWrite(indexingMode())) {
        // Compiled code asks for this before it stores in place.
        if (structure()->hasImmutableProperties()) [[unlikely]]
            return { };
        if (leastUpperBoundOfIndexingTypes(indexingType() & IndexingShapeMask, Int32Shape) == Int32Shape) {
            ASSERT(hasInt32(indexingMode()));
            convertFromCopyOnWrite(vm);
            return this->butterfly()->contiguousInt32();
        }
        return ContiguousJSValues();
    }

    if (structure()->hijacksIndexingHeader())
        return ContiguousJSValues();
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        if (indexingShouldBeSparse() || needsSlowPutIndexing()) [[unlikely]]
            return ContiguousJSValues();
        return createInitialInt32(vm, 0);
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        return convertUndecidedToInt32(vm);
        
    case ALL_DOUBLE_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES:
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return ContiguousJSValues();
        
    default:
        CRASH();
        return ContiguousJSValues();
    }
}

ContiguousDoubles JSObject::tryMakeWritableDoubleSlow(VM& vm)
{
    ASSERT(Options::allowDoubleShape());
    ASSERT(inherits(info()));

    if (isCopyOnWrite(indexingMode())) {
        // Compiled code asks for this before it stores in place.
        if (structure()->hasImmutableProperties()) [[unlikely]]
            return { };
        if (leastUpperBoundOfIndexingTypes(indexingType() & IndexingShapeMask, DoubleShape) == DoubleShape) {
            convertFromCopyOnWrite(vm);
            if (hasDouble(indexingMode()))
                return this->butterfly()->contiguousDouble();
            ASSERT(hasInt32(indexingMode()));
        } else
            return ContiguousDoubles();
    }

    if (structure()->hijacksIndexingHeader())
        return ContiguousDoubles();
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        if (indexingShouldBeSparse() || needsSlowPutIndexing()) [[unlikely]]
            return ContiguousDoubles();
        return createInitialDouble(vm, 0);
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        return convertUndecidedToDouble(vm);
        
    case ALL_INT32_INDEXING_TYPES:
        return convertInt32ToDouble(vm);
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return ContiguousDoubles();
        
    default:
        CRASH();
        return ContiguousDoubles();
    }
}

ContiguousJSValues JSObject::tryMakeWritableContiguousSlow(VM& vm)
{
    ASSERT(inherits(info()));

    if (isCopyOnWrite(indexingMode())) {
        // Compiled code asks for this before it stores in place.
        if (structure()->hasImmutableProperties()) [[unlikely]]
            return { };
        if (leastUpperBoundOfIndexingTypes(indexingType() & IndexingShapeMask, ContiguousShape) == ContiguousShape) {
            convertFromCopyOnWrite(vm);
            if (hasContiguous(indexingMode()))
                return this->butterfly()->contiguous();
            ASSERT(hasInt32(indexingMode()) || hasDouble(indexingMode()));
        } else
            return ContiguousJSValues();
    }

    if (structure()->hijacksIndexingHeader())
        return ContiguousJSValues();
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        if (indexingShouldBeSparse() || needsSlowPutIndexing()) [[unlikely]]
            return ContiguousJSValues();
        return createInitialContiguous(vm, 0);
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        return convertUndecidedToContiguous(vm);
        
    case ALL_INT32_INDEXING_TYPES:
        return convertInt32ToContiguous(vm);
        
    case ALL_DOUBLE_INDEXING_TYPES:
        return convertDoubleToContiguous(vm);
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return ContiguousJSValues();
        
    default:
        CRASH();
        return ContiguousJSValues();
    }
}

#if ASSERT_ENABLED
// An object that has elements when it becomes a prototype keeps the storage it has. What it is taken for counts for the objects
// below it; a store into a hole of its own asks the objects above it.
static bool storeIntoHoleNeedsSlowPutIndexing(const JSObject* object)
{
    if (!JSGlobalObject::isTakenToInterceptIndexedAccesses(object))
        return object->needsSlowPutIndexing();
    if (object->structure()->mayInterceptIndexedAccesses())
        return true;
    JSObject* prototype = object->getPrototypeDirect().getObject();
    return prototype && prototype->needsSlowPutIndexing();
}
#endif

ArrayStorage* JSObject::ensureArrayStorageSlow(VM& vm)
{
    // Compiled code asks for this before it stores elements in place. An object with immutable properties keeps the storage it has:
    // none (Object.prototype or Array.prototype with storage would make every array pay for it), or copy-on-write storage.
    if (structure()->hasImmutableIndexedProperties()) [[unlikely]]
        return nullptr;
    ASSERT(inherits(info()));

    if (objectTypesWithoutButterfly.contains(type())) [[unlikely]]
        return nullptr;
    if (structure()->hijacksIndexingHeader())
        return nullptr;

    ensureWritable(vm);

    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        if (indexingShouldBeSparse()) [[unlikely]]
            return ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm);
        return createInitialArrayStorage(vm);
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        ASSERT(!indexingShouldBeSparse());
        ASSERT(!storeIntoHoleNeedsSlowPutIndexing(this));
        return convertUndecidedToArrayStorage(vm);
        
    case ALL_INT32_INDEXING_TYPES:
        ASSERT(!indexingShouldBeSparse());
        ASSERT(!storeIntoHoleNeedsSlowPutIndexing(this));
        return convertInt32ToArrayStorage(vm);
        
    case ALL_DOUBLE_INDEXING_TYPES:
        ASSERT(!indexingShouldBeSparse());
        ASSERT(!storeIntoHoleNeedsSlowPutIndexing(this));
        return convertDoubleToArrayStorage(vm);
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        ASSERT(!indexingShouldBeSparse());
        ASSERT(!storeIntoHoleNeedsSlowPutIndexing(this));
        return convertContiguousToArrayStorage(vm);
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

ArrayStorage* JSObject::ensureArrayStorageExistsAndEnterDictionaryIndexingMode(VM& vm)
{
    ensureWritable(vm);

    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES: {
        createArrayStorage(vm, 0, 0);
        SparseArrayValueMap* map = allocateSparseIndexMap(vm);
        map->setSparseMode();
        return arrayStorage();
    }
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, convertUndecidedToArrayStorage(vm));
        
    case ALL_INT32_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, convertInt32ToArrayStorage(vm));
        
    case ALL_DOUBLE_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, convertDoubleToArrayStorage(vm));
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, convertContiguousToArrayStorage(vm));
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, this->butterfly()->arrayStorage());
        
    default:
        CRASH();
        return nullptr;
    }
}

void JSObject::switchToSlowPutArrayStorage(VM& vm)
{
    // Slow-put storage makes a store into a hole consult the prototype chain. Nothing is stored into an array with immutable
    // properties, and its copy-on-write elements stay readable in place.
    if (!tryMakeWritable(vm)) [[unlikely]]
        return;

    switch (indexingType()) {
    case ArrayClass:
        // ensureArrayStorage() gives an object with immutable properties none, because compiled code asks for it before it stores
        // elements in place. This conversion is the engine's own. The storage it makes here is the kind every other such object
        // has: no capacity, and a sparse map in sparse mode, which is what makes a put consult extensibility.
        if (structure()->hasImmutableIndexedProperties()) [[unlikely]]
            ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm);
        else
            ensureArrayStorage(vm);
        RELEASE_ASSERT(hasAnyArrayStorage(indexingType()));
        if (hasSlowPutArrayStorage(indexingType()))
            return;
        switchToSlowPutArrayStorage(vm);
        break;

    case ALL_UNDECIDED_INDEXING_TYPES:
        convertUndecidedToArrayStorage(vm, TransitionKind::AllocateSlowPutArrayStorage);
        break;
        
    case ALL_INT32_INDEXING_TYPES:
        convertInt32ToArrayStorage(vm, TransitionKind::AllocateSlowPutArrayStorage);
        break;
        
    case ALL_DOUBLE_INDEXING_TYPES:
        convertDoubleToArrayStorage(vm, TransitionKind::AllocateSlowPutArrayStorage);
        break;
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        convertContiguousToArrayStorage(vm, TransitionKind::AllocateSlowPutArrayStorage);
        break;
        
    case NonArrayWithArrayStorage:
    case ArrayWithArrayStorage: {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::SwitchToSlowPutArrayStorage, &deferred);
        setStructure(vm, newStructure);
        break;
    }
        
    default:
        CRASH();
        break;
    }
}

void JSObject::setPrototypeDirect(VM& vm, JSValue prototype)
{
    ASSERT(prototype.isObject() || prototype.isNull());
    if (structure()->isCompiledStructure() || type() == SlimClosureType) [[unlikely]] {
        if (getPrototypeDirect() != prototype)
            methodTable()->setPrototype(this, realm(), prototype, false);
        return;
    }
    // (No result to give. setPrototypeWithCycleCheck() has refused before it comes here; this is for a caller that does not ask.)
    if (structure()->hasImmutablePrototype()) [[unlikely]]
        return;
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    else if (!prototype.isNull()) [[unlikely]] // Conservative hardening.
        return;

    bool chainAlreadyMayInterceptIndexedAccesses = mayBePrototype() && anyObjectInChainMayInterceptIndexedAccesses();

    if (structure()->hasMonoProto()) {
        DeferredStructureTransitionWatchpointFire deferred(vm, structure());
        Structure* newStructure = Structure::changePrototypeTransition(vm, structure(), prototype, deferred);
        setStructure(vm, newStructure);
        // Prototype-chain gets changed for the already cached structures. Invalidate the cache.
        if (mayBePrototype()) [[unlikely]]
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Prototype, this);
    } else
        putDirectOffset(vm, knownPolyProtoOffset, prototype);

    if (!anyObjectInChainMayInterceptIndexedAccesses())
        return;

    // Realm is always non-nullptr since realmless Structure's objects (e.g. WasmGC Struct) cannot call setPrototypeDirect.
    if (mayBePrototype() && !chainAlreadyMayInterceptIndexedAccesses) {
        realm()->haveABadTime(vm, this);
        return;
    }

    if (!hasIndexedProperties(indexingType()))
        return;
    
    if (shouldUseSlowPut(indexingType()))
        return;
    
    switchToSlowPutArrayStorage(vm);
}

bool JSObject::setPrototypeWithCycleCheck(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (this->structure()->isImmutablePrototypeExoticObject()) {
        // This implements https://tc39.github.io/ecma262/#sec-set-immutable-prototype.
        if (this->getPrototype(globalObject) == prototype)
            return true;

        return typeError(globalObject, scope, shouldThrowIfCantSet, "Cannot set prototype of immutable prototype object"_s);
    }

    if (this->structure()->hasFrozenElementsInPlace()) [[unlikely]]
        moveFrozenElementsToSparseMap(vm);

    if (this->structure()->hasImmutablePrototype()) [[unlikely]] {
        if (this->getPrototypeDirect() == prototype)
            return true;
#if ENABLE(AOT)
        AOT::RuntimeHooks::didRefuseChange(globalObject, this, nullptr, AOT::RefusedChange::Prototype);
#endif
        ImmutablePropertyNames* names = this->structure()->listedImmutablePropertyNames();
        String messageOfOwner = names && shouldThrowIfCantSet ? names->messageOfRefusedPrototype() : String();
        if (!messageOfOwner.isNull())
            return typeError(globalObject, scope, true, messageOfOwner);
        return typeError(globalObject, scope, shouldThrowIfCantSet, "Cannot set prototype of object with immutable properties"_s);
    }

    // Default realm global objects should have mutable prototypes despite having
    // a Proxy globalThis.
    ASSERT(this->isGlobalObject() || JSValue(this).toThis(globalObject, ECMAMode::sloppy()) == this);

    if (this->getPrototypeDirect() == prototype)
        return true;

    bool isExtensible = this->isExtensible(globalObject);
    RETURN_IF_EXCEPTION(scope, false);

    if (!isExtensible)
        return typeError(globalObject, scope, shouldThrowIfCantSet, ReadonlyPropertyWriteError);

    // Some clients would have already done this check because of the order of the check
    // specified in their respective specifications. However, we still do this check here
    // to document and enforce this invariant about the nature of prototype.
    if (!prototype.isObject() && !prototype.isNull()) [[unlikely]]
        return typeError(globalObject, scope, shouldThrowIfCantSet, PrototypeValueCanOnlyBeAnObjectOrNullTypeError);

    JSValue nextPrototype = prototype;
    while (nextPrototype && nextPrototype.isObject()) {
        if (nextPrototype == this)
            return typeError(globalObject, scope, shouldThrowIfCantSet, "cyclic __proto__ value"_s);
        // FIXME: The specification currently says we should check if the [[GetPrototypeOf]] internal method of nextPrototype
        // is not the ordinary object internal method. However, we currently restrict this to Proxy objects as it would allow
        // for cycles with certain HTML objects (WindowProxy, Location) otherwise.
        // https://bugs.webkit.org/show_bug.cgi?id=161534
        if (asObject(nextPrototype)->type() == ProxyObjectType) [[unlikely]]
            break; // We're done. Set the prototype.
        nextPrototype = asObject(nextPrototype)->getPrototypeDirect();
    }
    setPrototypeDirect(vm, prototype);
    return true;
}

bool JSObject::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    ASSERT(!object->structure()->isCompiledStructure());
    return object->setPrototypeWithCycleCheck(globalObject->vm(), globalObject, prototype, shouldThrowIfCantSet);
}

JSValue JSObject::getPrototype(JSObject* object, JSGlobalObject*)
{
    return object->getPrototypeDirect();
}

bool JSObject::setPrototype(VM&, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    return methodTable()->setPrototype(this, globalObject, prototype, shouldThrowIfCantSet);
}

// llint_aot_op_new_object may have made a compiled object of the literal's first members.
static bool defineAccessorOfLiteral(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor)
{
    if (object->structure()->isCompiledStructure()) [[unlikely]]
        return object->methodTable()->defineOwnProperty(object, globalObject, propertyName, descriptor, true);
    return JSObject::defineOwnProperty(object, globalObject, propertyName, descriptor, true);
}

bool JSObject::putGetter(JSGlobalObject* globalObject, PropertyName propertyName, JSValue getter, unsigned attributes)
{
    PropertyDescriptor descriptor;
    descriptor.setGetter(getter);

    ASSERT(attributes & PropertyAttribute::Accessor);
    if (!(attributes & PropertyAttribute::ReadOnly))
        descriptor.setConfigurable(true);
    if (!(attributes & PropertyAttribute::DontEnum))
        descriptor.setEnumerable(true);

    return defineAccessorOfLiteral(this, globalObject, propertyName, descriptor);
}

bool JSObject::putSetter(JSGlobalObject* globalObject, PropertyName propertyName, JSValue setter, unsigned attributes)
{
    PropertyDescriptor descriptor;
    descriptor.setSetter(setter);

    ASSERT(attributes & PropertyAttribute::Accessor);
    if (!(attributes & PropertyAttribute::ReadOnly))
        descriptor.setConfigurable(true);
    if (!(attributes & PropertyAttribute::DontEnum))
        descriptor.setEnumerable(true);

    return defineAccessorOfLiteral(this, globalObject, propertyName, descriptor);
}

bool JSObject::putDirectAccessor(JSGlobalObject* globalObject, PropertyName propertyName, GetterSetter* accessor, unsigned attributes)
{
    ASSERT(attributes & PropertyAttribute::Accessor);

    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return putDirectIndex(globalObject, index.value(), accessor, attributes, PutDirectIndexLikePutDirect);

    return putDirectNonIndexAccessor(globalObject->vm(), propertyName, accessor, attributes);
}

// FIXME: Introduce a JSObject::putDirectCustomValue() method instead of using
// JSObject::putDirectCustomAccessor() to put CustomValues.
// https://bugs.webkit.org/show_bug.cgi?id=192576
bool JSObject::putDirectCustomAccessor(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes)
{
    ASSERT(!parseIndex(propertyName));
    ASSERT(value.isCustomGetterSetter());
    if (!(attributes & PropertyAttribute::CustomAccessor))
        attributes |= PropertyAttribute::CustomValue;

#if ENABLE(AOT)
    if (structure()->isCompiledStructure()) [[unlikely]] {
        AOT::PropertyWalk::putDirect(vm, this, propertyName, value, attributes);
        return true;
    }
#endif

#if ASSERT_ENABLED
    bool hadProperty = isValidOffset(structure()->get(vm, propertyName));
#endif
    PutPropertySlot slot(this);
    bool result = putDirectInternal<PutModeDefineOwnProperty>(vm, propertyName, value, attributes, slot).isNull();
    // (Refused, for an object with immutable properties: nothing was put, so there is nothing to record on the Structure.)
    if (!result) [[unlikely]]
        return false;

    // The slot says nothing about an addition that no cache may keep (setStructureForAddedProperty()).
    ASSERT(!hadProperty);

    Structure* structure = this->structure();
    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();
    structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
    return result;
}

#if ENABLE(AOT)
JSValue JSObject::getDirectFromCompiledObject(VM& vm, PropertyName propertyName, unsigned* attributesOrNull) const
{
    if (type() != CompiledObjectType)
        return { };
    return AOT::PropertyWalk::getDirect(vm, this, propertyName, attributesOrNull);
}

ASCIILiteral JSObject::putDirectToCompiledObject(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes, PutMode mode, PutPropertySlot& slot)
{
    slot.disableCaching();
    if (AOT::CompiledCallee::reifyProperties(vm, this)) {
        if (mode == PutModePut)
            return putDirectInternal<PutModePut>(vm, propertyName, value, attributes, slot);
        return putDirectInternal<PutModeDefineOwnProperty>(vm, propertyName, value, attributes, slot);
    }
    RELEASE_ASSERT(type() == CompiledObjectType);
    if (mode == PutModePut) {
        unsigned currentAttributes;
        if (AOT::PropertyWalk::getDirect(vm, this, propertyName, &currentAttributes)) {
            if (currentAttributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor)
                return ReadonlyPropertyChangeError;
            ASSERT(!(currentAttributes & PropertyAttribute::AccessorOrCustomAccessorOrValue));
            attributes = currentAttributes;
        } else if (AOT::PropertyWalk::testIntegrity(this, AOT::PropertyWalk::Integrity::NonExtensible) != AOT::PropertyWalk::Presence::Absent)
            return NonExtensibleObjectPropertyDefineError;
    }
    AOT::PropertyWalk::putDirect(vm, this, propertyName, value, attributes);
    return { };
}

ASCIILiteral JSObject::putDirectToCompiledClosure(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes, PutPropertySlot& slot)
{
    slot.disableCaching();
    AOT::PropertyWalk::willPutDirectOnClosure(vm, this, propertyName.uid());
    ASCIILiteral error = putDirectInternal<PutModeDefineOwnPropertyWithoutClosureHooks>(vm, propertyName, value, attributes, slot);
    if (error.isNull())
        AOT::PropertyWalk::didPutDirectOnClosure(vm, this, propertyName.uid());
    return error;
}
#endif

void JSObject::putDirectCustomGetterSetterWithoutTransition(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes)
{
    ASSERT(!parseIndex(propertyName));
    ASSERT(value.isCustomGetterSetter());
    ASSERT(attributes & PropertyAttribute::CustomAccessorOrValue);
    // (No result to give: the caller is initializing an object it takes to be new, and the property is not put.)
    if (structure()->isImmutableProperty(propertyName.uid()) && !vm.allowLazyMaterializationOfImmutablePropertiesCount) [[unlikely]]
        return;

    StructureID structureID = this->structureID();
    Structure* structure = structureID.decode();
    PropertyOffset offset = prepareToPutDirectWithoutTransition(vm, propertyName, attributes, structureID, structure);
    putDirectOffset(vm, offset, value);

    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();
    structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
}

bool JSObject::putDirectNonIndexAccessor(VM& vm, PropertyName propertyName, GetterSetter* accessor, unsigned attributes)
{
    ASSERT(attributes & PropertyAttribute::Accessor);
    PutPropertySlot slot(this);
    bool result = putDirectInternal<PutModeDefineOwnProperty>(vm, propertyName, accessor, attributes, slot).isNull();
    // (As in putDirectCustomAccessor().)
    if (!result) [[unlikely]]
        return false;

    Structure* structure = this->structure();
    if (structure->isCompiledStructure()) [[unlikely]]
        return true;
    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();

    structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
    return result;
}

void JSObject::putDirectNonIndexAccessorWithoutTransition(VM& vm, PropertyName propertyName, GetterSetter* accessor, unsigned attributes)
{
    ASSERT(attributes & PropertyAttribute::Accessor);
    // (As above.)
    if (structure()->isImmutableProperty(propertyName.uid()) && !vm.allowLazyMaterializationOfImmutablePropertiesCount) [[unlikely]]
        return;
    StructureID structureID = this->structureID();
    Structure* structure = structureID.decode();
    PropertyOffset offset = prepareToPutDirectWithoutTransition(vm, propertyName, attributes, structureID, structure);
    putDirectOffset(vm, offset, accessor);
    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();

    structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
}

// https://tc39.es/ecma262/#sec-hasproperty
bool JSObject::hasProperty(JSGlobalObject* globalObject, PropertyName propertyName) const
{
    PropertySlot slot(this, PropertySlot::InternalMethodType::HasProperty);
    return const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
}

bool JSObject::hasProperty(JSGlobalObject* globalObject, unsigned propertyName) const
{
    PropertySlot slot(this, PropertySlot::InternalMethodType::HasProperty);
    return const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
}

bool JSObject::hasProperty(JSGlobalObject* globalObject, uint64_t propertyName) const
{
    if (propertyName <= MAX_ARRAY_INDEX) [[likely]]
        return hasProperty(globalObject, static_cast<uint32_t>(propertyName));
    ASSERT(propertyName <= maxSafeInteger());
    return hasProperty(globalObject, Identifier::from(globalObject->vm(), propertyName));
}

bool JSObject::hasEnumerableProperty(JSGlobalObject* globalObject, PropertyName propertyName) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, false);
    if (!hasProperty)
        return false;
    return !(slot.attributes() & PropertyAttribute::DontEnum) || (slot.slotBase() && slot.slotBase()->structure()->typeInfo().getOwnPropertySlotMayBeWrongAboutDontEnum());
}

bool JSObject::hasEnumerableProperty(JSGlobalObject* globalObject, unsigned propertyName) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, false);
    if (!hasProperty)
        return false;
    return !(slot.attributes() & PropertyAttribute::DontEnum) || (slot.slotBase() && slot.slotBase()->structure()->typeInfo().getOwnPropertySlotMayBeWrongAboutDontEnum());
}

// ECMA 8.6.2.5
bool JSObject::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    ASSERT(!cell->structure()->isCompiledStructure());
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    VM& vm = globalObject->vm();

#if ENABLE(AOT)
    if (vm.aotState().hasObjectsBeingFilled) [[unlikely]]
        AOT::RuntimeHooks::finishFillingObjects(vm);
#endif

    // Every property the object has stays. Deleting one it does not have succeeds, as it does for any object.
    if (thisObject->structure()->isImmutableProperty(propertyName.uid())) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        PropertySlot found(thisObject, PropertySlot::InternalMethodType::GetOwnProperty);
        bool isRefused = thisObject->methodTable()->getOwnPropertySlot(thisObject, globalObject, propertyName, found);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isRefused)
            return true;
#if ENABLE(AOT)
        AOT::RuntimeHooks::didRefuseChange(globalObject, thisObject, propertyName.uid(), AOT::RefusedChange::Deletion);
#endif
        // The language refuses the deletion of a property that reads as not configurable, and the operator says so in strict code alone.
        if (slot.answersRefusal() || found.attributes() & PropertyAttribute::DontDelete)
            return false;
        RefusalOfOwner refusal = refusalOfOwner(globalObject, thisObject, propertyName.uid(), true);
        if (!refusal.alwaysThrows)
            return false;
        return typeError(globalObject, scope, true, refusal.message.isNull() ? String(UnableToDeletePropertyError) : refusal.message);
    }
    
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return thisObject->methodTable()->deletePropertyByIndex(thisObject, globalObject, index.value());

    unsigned attributes;

    if (thisObject->hasNonReifiedStaticProperties()) {
        if (auto entry = thisObject->findPropertyHashEntry(propertyName)) {
            // If the static table contains a non-configurable (DontDelete) property then we can return early;
            // if there is a property in the storage array it too must be non-configurable (the language does
            // not allow repacement of a non-configurable property with a configurable one).
            if (entry->value->attributes() & PropertyAttribute::DontDelete && vm.deletePropertyMode() != VM::DeletePropertyMode::IgnoreConfigurable) {
                ASSERT(!isValidOffset(thisObject->structure()->get(vm, propertyName, attributes)) || attributes & PropertyAttribute::DontDelete);
                return false;
            }
            thisObject->reifyAllStaticProperties(globalObject);
        }
    }

    Structure* structure = thisObject->structure();

    bool propertyIsPresent = isValidOffset(structure->get(vm, propertyName, attributes));
    if (propertyIsPresent) {
        if (attributes & PropertyAttribute::DontDelete && vm.deletePropertyMode() != VM::DeletePropertyMode::IgnoreConfigurable) {
            slot.setNonconfigurable();
            return false;
        }

        PropertyOffset offset = invalidOffset;
        if (structure->isUncacheableDictionary()) {
            offset = structure->removePropertyWithoutTransition(vm, propertyName, [] (const GCSafeConcurrentJSLocker&, PropertyOffset, PropertyOffset) { });
            ASSERT(!isValidOffset(structure->get(vm, propertyName, attributes)));
            if (offset != invalidOffset)
                thisObject->locationForOffset(offset)->clear();
        } else {
            DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, structure);
            structure = Structure::removePropertyTransition(vm, structure, propertyName, offset, &deferredWatchpointFire);
            slot.setHit(offset);
            ASSERT(structure->outOfLineCapacity() || !thisObject->structure()->outOfLineCapacity());
            thisObject->setStructure(vm, structure);
            ASSERT(!isValidOffset(structure->get(vm, propertyName, attributes)));
            if (offset != invalidOffset)
                thisObject->locationForOffset(offset)->clear();
        }
        if (thisObject->mayBePrototype()) [[unlikely]]
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Remove, thisObject, propertyName.uid());
    } else
        slot.setConfigurableMiss();

    return true;
}

bool JSObject::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned i)
{
    VM& vm = globalObject->vm();
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);

    if (thisObject->structure()->hasImmutableIndexedProperties()) [[unlikely]]
        return !thisObject->hasOwnProperty(globalObject, i);
    
    if (i > MAX_ARRAY_INDEX)
        return JSCell::deleteProperty(thisObject, globalObject, Identifier::from(vm, i));
    
    switch (thisObject->indexingMode()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        return true;

    case CopyOnWriteArrayWithInt32:
    case CopyOnWriteArrayWithContiguous: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return true;
        thisObject->convertFromCopyOnWrite(vm);
        [[fallthrough]];
    }

    case ALL_WRITABLE_INT32_INDEXING_TYPES:
    case ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return true;
        butterfly->contiguous().at(thisObject, i).clear();
        return true;
    }

    case CopyOnWriteArrayWithDouble: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return true;
        thisObject->convertFromCopyOnWrite(vm);
        [[fallthrough]];
    }

    case ALL_WRITABLE_DOUBLE_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return true;
        butterfly->contiguousDouble().at(thisObject, i) = PNaN;
        return true;
    }
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = thisObject->butterfly()->arrayStorage();
        
        if (i < storage->vectorLength()) {
            WriteBarrier<Unknown>& valueSlot = storage->m_vector[i];
            if (valueSlot) {
                valueSlot.clear();
                --storage->m_numValuesInVector;
            }
        } else if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
            SparseArrayValueMap::iterator it = map->find(i);
            if (it != map->notFound()) {
                if (it->attributes() & PropertyAttribute::DontDelete)
                    return false;
                map->remove(it);
            }
        }
        
        return true;
    }
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
}

template<CachedSpecialPropertyKey key>
static ALWAYS_INLINE JSValue callToPrimitiveFunction(JSGlobalObject* globalObject, const JSObject* object, PropertyName propertyName, PreferredPrimitiveType hint)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue function = object->structure()->cachedSpecialProperty(key);
    if (!function) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
        // FIXME: Remove this when we have fixed: rdar://problem/33451840
        // https://bugs.webkit.org/show_bug.cgi?id=187109.
        constexpr bool debugNullStructure = key == CachedSpecialPropertyKey::ToPrimitive;
        bool hasProperty = const_cast<JSObject*>(object)->getPropertySlot<debugNullStructure>(globalObject, propertyName, slot);
        RETURN_IF_EXCEPTION(scope, scope.exception());
        function = hasProperty ? slot.getValue(globalObject, propertyName) : jsUndefined();
        RETURN_IF_EXCEPTION(scope, scope.exception());
        object->structure()->cacheSpecialProperty(globalObject, vm, function, key, slot);
        RETURN_IF_EXCEPTION(scope, scope.exception());
    }
    if (function.isUndefinedOrNull())
        return JSValue();

    // Add optimizations for frequently called functions.
    // https://bugs.webkit.org/show_bug.cgi?id=216084
    if constexpr (key == CachedSpecialPropertyKey::ToString) {
        if (function == globalObject->objectProtoToStringFunction()) {
            if (auto result = object->structure()->cachedSpecialProperty(CachedSpecialPropertyKey::ToStringTag))
                return result;
        }
    }

    if constexpr (key == CachedSpecialPropertyKey::ValueOf) {
        if (function == globalObject->objectProtoValueOfFunction())
            return JSValue();
    }

    auto callData = JSC::getCallDataInline(function);
    if (callData.type == CallData::Type::None) {
        if constexpr (key == CachedSpecialPropertyKey::ToPrimitive)
            throwTypeError(globalObject, scope, "Symbol.toPrimitive is not a function, undefined, or null"_s);
        return scope.exception();
    }

    constexpr size_t argCount = key == CachedSpecialPropertyKey::ToPrimitive ? 1 : 0;
    std::array<EncodedJSValue, argCount> callArgs { };
    if constexpr (key == CachedSpecialPropertyKey::ToPrimitive) {
        JSString* hintString = nullptr;
        switch (hint) {
        case NoPreference:
            hintString = vm.smallStrings.defaultString();
            break;
        case PreferNumber:
            hintString = vm.smallStrings.numberString();
            break;
        case PreferString:
            hintString = vm.smallStrings.stringString();
            break;
        }
        callArgs[0] = JSValue::encode(hintString);
    } else {
        UNUSED_PARAM(hint);
    }

    JSValue result = call(globalObject, function, callData, const_cast<JSObject*>(object), ArgList { callArgs.data(), callArgs.size() });
    RETURN_IF_EXCEPTION(scope, scope.exception());
    ASSERT(!result.isGetterSetter());
    if (result.isObject()) {
        if constexpr (key == CachedSpecialPropertyKey::ToPrimitive)
            return throwTypeError(globalObject, scope, "Symbol.toPrimitive returned an object"_s);
        return JSValue();
    }
    return result;
}

// ECMA 7.1.1
JSValue JSObject::ordinaryToPrimitive(JSGlobalObject* globalObject, PreferredPrimitiveType hint) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Make sure that whatever default value methods there are on object's prototype chain are
    // being watched.
    // FIXME: Remove this hack for DFG.
    // https://bugs.webkit.org/show_bug.cgi?id=216117
    for (const JSObject* object = this; object; object = object->structure()->storedPrototypeObject(object))
        object->structure()->startWatchingInternalPropertiesIfNecessary(vm);

    JSValue value;
    if (hint == PreferString) {
        value = callToPrimitiveFunction<CachedSpecialPropertyKey::ToString>(globalObject, this, vm.propertyNames->toString, hint);
        EXCEPTION_ASSERT(!scope.exception() || scope.exception() == value.asCell());
        if (value)
            return value;
        value = callToPrimitiveFunction<CachedSpecialPropertyKey::ValueOf>(globalObject, this, vm.propertyNames->valueOf, hint);
        EXCEPTION_ASSERT(!scope.exception() || scope.exception() == value.asCell());
        if (value)
            return value;
    } else {
        value = callToPrimitiveFunction<CachedSpecialPropertyKey::ValueOf>(globalObject, this, vm.propertyNames->valueOf, hint);
        EXCEPTION_ASSERT(!scope.exception() || scope.exception() == value.asCell());
        if (value)
            return value;
        value = callToPrimitiveFunction<CachedSpecialPropertyKey::ToString>(globalObject, this, vm.propertyNames->toString, hint);
        EXCEPTION_ASSERT(!scope.exception() || scope.exception() == value.asCell());
        if (value)
            return value;
    }

    return throwTypeError(globalObject, scope, "No default value"_s);
}

JSValue JSObject::toPrimitive(JSGlobalObject* globalObject, PreferredPrimitiveType preferredType) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isJSArray(this)) {
        auto* array = uncheckedDowncast<JSArray>(const_cast<JSObject*>(this));
        if (array->isToPrimitiveFastAndNonObservable()) [[likely]]
            RELEASE_AND_RETURN(scope, array->fastToString(globalObject));
    }

    // For a plain default object ordinaryToPrimitive collapses to the cached ToStringTag string
    // for both hints (primordial valueOf is skipped), so returning it here matches the slow path.
    if (auto* tag = structure()->defaultToPrimitiveFastAndNonObservable(vm))
        return tag;

    JSValue value = callToPrimitiveFunction<CachedSpecialPropertyKey::ToPrimitive>(globalObject, this, vm.propertyNames->toPrimitiveSymbol, preferredType);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;

    RELEASE_AND_RETURN(scope, ordinaryToPrimitive(globalObject, preferredType));
}

bool JSObject::getOwnStaticPropertySlot(VM& vm, PropertyName propertyName, PropertySlot& slot)
{
    for (auto* info = classInfo(); info; info = info->parentClass) {
        if (auto* table = info->staticPropHashTable) {
            if (getStaticPropertySlotFromTable(vm, table->classForThis, *table, this, propertyName, slot))
                return true;
        }
    }
    return false;
}

std::optional<Structure::PropertyHashEntry> JSObject::findPropertyHashEntry(PropertyName propertyName) const
{
    return structure()->findPropertyHashEntry(propertyName);
}

bool JSObject::hasInstance(JSGlobalObject* globalObject, JSValue value, JSValue hasInstanceValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!hasInstanceValue.isUndefinedOrNull() && hasInstanceValue != globalObject->functionProtoHasInstanceSymbolFunction()) {
        auto callData = JSC::getCallDataInline(hasInstanceValue);
        if (callData.type == CallData::Type::None) {
            throwException(globalObject, scope, createInvalidInstanceofParameterErrorHasInstanceValueNotFunction(globalObject, this));
            return false;
        }

        auto args = WTF::toArray<EncodedJSValue>({
            JSValue::encode(value),
        });
        JSValue result = call(globalObject, hasInstanceValue, callData, this, ArgList { args.data(), args.size() });
        RETURN_IF_EXCEPTION(scope, false);
        return result.toBoolean(globalObject);
    }

    TypeInfo info = structure()->typeInfo();
    if (info.implementsDefaultHasInstance()) {
        if (!value.isObject())
            return false;
        JSValue prototype = get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, defaultHasInstance(globalObject, value, prototype));
    }
    if (info.implementsHasInstance()) {
        if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
            throwStackOverflowError(globalObject, scope);
            return false;
        }
        RELEASE_AND_RETURN(scope, methodTable()->customHasInstance(this, globalObject, value));
    }

    throwException(globalObject, scope, createInvalidInstanceofParameterErrorNotFunction(globalObject, this));
    return false;
}

bool JSObject::hasInstance(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue hasInstanceValue = get(globalObject, vm.propertyNames->hasInstanceSymbol);
    RETURN_IF_EXCEPTION(scope, false);

    RELEASE_AND_RETURN(scope, hasInstance(globalObject, value, hasInstanceValue));
}

bool JSObject::defaultHasInstance(JSGlobalObject* globalObject, JSValue value, JSValue proto)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!value.isObject())
        return false;

    if (!proto.isObject()) {
        throwTypeError(globalObject, scope, "instanceof called on an object with an invalid prototype property."_s);
        return false;
    }

    JSObject* object = asObject(value);
    while (true) {
        JSValue objectValue = object->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!objectValue.isObject())
            return false;
        object = asObject(objectValue);
        if (proto == object)
            return true;
    }
    ASSERT_NOT_REACHED();
}

JSC_DEFINE_HOST_FUNCTION(objectPrivateFuncInstanceOf, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSValue value = callFrame->uncheckedArgument(0);
    JSValue proto = callFrame->uncheckedArgument(1);

    return JSValue::encode(jsBoolean(JSObject::defaultHasInstance(globalObject, value, proto)));
}

void JSObject::getPropertyNames(JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* object = this;
    unsigned prototypeCount = 0;

    while (true) {
        object->methodTable()->getOwnPropertyNames(object, globalObject, propertyNames, mode);
        RETURN_IF_EXCEPTION(scope, void());

        JSValue prototype = object->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (prototype.isNull())
            break;

        if (++prototypeCount > maximumPrototypeChainDepth) [[unlikely]] {
            throwStackOverflowError(globalObject, scope);
            return;
        }

        object = asObject(prototype);
    }
}

void JSObject::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    ASSERT(!object->structure()->isCompiledStructure());
    object->getOwnIndexedPropertyNames(globalObject, propertyNames, mode);
    object->getOwnNonIndexPropertyNames(globalObject, propertyNames, mode);
}

void JSObject::getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode)
{
    // Structure::validateFlags() breaks if this method isn't exported, which is impossible if it's inlined.
}

void JSObject::getOwnIndexedPropertyNames(JSGlobalObject*, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    JSObject* object = this;

    if (propertyNames.includeStringProperties()) {
        // Add numeric properties first per step 2 of https://tc39.es/ecma262/#sec-ordinaryownpropertykeys
        // FIXME: Filling PropertyNameArray with an identifier for every integer
        // is incredibly inefficient for large arrays. We need a different approach,
        // which almost certainly means a different structure for PropertyNameArray.
        switch (object->indexingType()) {
        case ALL_BLANK_INDEXING_TYPES:
        case ALL_UNDECIDED_INDEXING_TYPES:
            break;
            
        case ALL_INT32_INDEXING_TYPES:
        case ALL_CONTIGUOUS_INDEXING_TYPES: {
            Butterfly* butterfly = object->butterfly();
            unsigned usedLength = butterfly->publicLength();
            for (unsigned i = 0; i < usedLength; ++i) {
                if (!butterfly->contiguous().at(object, i))
                    continue;
                propertyNames.add(i);
            }
            break;
        }
            
        case ALL_DOUBLE_INDEXING_TYPES: {
            Butterfly* butterfly = object->butterfly();
            unsigned usedLength = butterfly->publicLength();
            for (unsigned i = 0; i < usedLength; ++i) {
                double value = butterfly->contiguousDouble().at(object, i);
                if (value != value)
                    continue;
                propertyNames.add(i);
            }
            break;
        }
            
        case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
            ArrayStorage* storage = object->butterfly()->arrayStorage();
            
            unsigned usedVectorLength = std::min(storage->length(), storage->vectorLength());
            for (unsigned i = 0; i < usedVectorLength; ++i) {
                if (storage->m_vector[i])
                    propertyNames.add(i);
            }
            
            if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
                auto keys = WTF::compactMap<0, UnsafeVectorOverflow>(*map, [mode](auto& entry) ->std::optional<unsigned> {
                    if (mode == DontEnumPropertiesMode::Include || !(entry.attributes() & PropertyAttribute::DontEnum))
                        return entry.index();
                    return std::nullopt;
                });
                
                std::ranges::sort(keys);
                for (unsigned i = 0; i < keys.size(); ++i)
                    propertyNames.add(keys[i]);
            }
            break;
        }
            
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
}

void JSObject::getOwnNonIndexPropertyNames(JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    ASSERT(!structure()->isCompiledStructure());
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    methodTable()->getOwnSpecialPropertyNames(this, globalObject, propertyNames, mode);
    RETURN_IF_EXCEPTION(scope, void());

    scope.release();
    getNonReifiedStaticPropertyNames(vm, propertyNames, mode);
    structure()->getPropertyNamesFromStructure(vm, propertyNames, mode);
}

double JSObject::toNumber(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue primitive = toPrimitive(globalObject, PreferNumber);
    RETURN_IF_EXCEPTION(scope, 0.0); // should be picked up soon in Nodes.cpp
    RELEASE_AND_RETURN(scope, primitive.toNumber(globalObject));
}

JSString* JSObject::toString(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isJSArray(this)) {
        auto* array = uncheckedDowncast<JSArray>(const_cast<JSObject*>(this));
        if (array->isToPrimitiveFastAndNonObservable()) [[likely]]
            RELEASE_AND_RETURN(scope, array->fastToString(globalObject));
    }

    if (auto* tag = structure()->defaultToPrimitiveFastAndNonObservable(vm))
        return tag;

    JSValue primitive = callToPrimitiveFunction<CachedSpecialPropertyKey::ToPrimitive>(globalObject, this, vm.propertyNames->toPrimitiveSymbol, PreferString);
    RETURN_IF_EXCEPTION(scope, jsEmptyString(vm));
    if (!primitive) [[likely]] {
        primitive = ordinaryToPrimitive(globalObject, PreferString);
        RETURN_IF_EXCEPTION(scope, jsEmptyString(vm));
    }

    RELEASE_AND_RETURN(scope, primitive.toString(globalObject));
}

void JSObject::seal(VM& vm)
{
    // The attributes do not change. Object.seal() does not come here for such an object: it takes the generic path, which reports the refusal.
    if (structure()->hasImmutableProperties()) [[unlikely]]
        return;
#if ENABLE(AOT)
    if (structure()->isCompiledStructure() || type() == SlimClosureType) [[unlikely]] {
        if (!AOT::CompiledCallee::reifyProperties(vm, this)) {
            AOT::PropertyWalk::setIntegrity(vm, this, AOT::PropertyWalk::Integrity::Sealed);
            return;
        }
    }
#endif
    if (isSealed(vm))
        return;
    materializeLazyOwnProperties(vm);
    enterDictionaryIndexingMode(vm);
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::sealTransition(vm, oldStructure, &deferred));
    }
}

void JSObject::freeze(VM& vm)
{
    // The attributes do not change. Object.freeze() does not come here for such an object: it takes the generic path, which reports the refusal.
    if (structure()->hasImmutableProperties()) [[unlikely]]
        return;
#if ENABLE(AOT)
    if (structure()->isCompiledStructure() || type() == SlimClosureType) [[unlikely]] {
        if (!AOT::CompiledCallee::reifyProperties(vm, this)) {
            AOT::PropertyWalk::setIntegrity(vm, this, AOT::PropertyWalk::Integrity::Frozen);
            return;
        }
    }
#endif
    if (isFrozen(vm))
        return;
    materializeLazyOwnProperties(vm);
    enterDictionaryIndexingMode(vm);
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::freezeTransition(vm, oldStructure, &deferred));
        if (mayBePrototype()) [[unlikely]]
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change, this);
    }
}

void JSObject::materializeLazyOwnProperties(VM& vm)
{
    if (!structure()->typeInfo().overridesGetOwnSpecialPropertyNames())
        return;

    // Force reifying lazy properties. Special properties (e.g. function "length" / "name",
    // or "arguments" / "caller") are materialized onto the object as a side effect of
    // enumerating them via getOwnPropertyNames, so the call below is what does the reification.
    JSGlobalObject* globalObject = this->realm();
    PropertyNameArrayBuilder propertyNames(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    methodTable()->getOwnPropertyNames(this, globalObject, propertyNames, DontEnumPropertiesMode::Include);
    scope.releaseAssertNoExceptionExceptTermination();
}

// A class that overrides a write hook can change the object before, or without, reaching JSObject's checks. The classes listed here
// do not. ErrorInstance, StringObject, RegExpObject and ClonedArguments only materialize lazy properties or refuse on their own
// account before they call JSObject's implementation (RegExpObject because makePropertiesImmutable() makes lastIndex non-writable).
// JSArray and JSFunction test hasImmutableProperties() where they write directly. JSGlobalObject's hooks only add its variables,
// which makePropertiesImmutable() makes read-only.
static bool canMakePropertiesImmutable(JSObject* object)
{
    // Not DirectArguments or ScopedArguments: a mapped element is a view of the function's parameter variable, which the function
    // can still assign.
    if (object->structure()->typeInfo().writeHooksHonorImmutableProperties())
        return true;
    const MethodTable* methodTable = object->methodTable();
    for (const ClassInfo* classInfo : { JSObject::info(), JSArray::info(), JSFunction::info(), InternalFunction::info(), ErrorInstance::info(), RegExpObject::info(), StringObject::info(), ClonedArguments::info(), JSGlobalObject::info() }) {
        // The object's write hooks have to be this class's own: nothing is known about a hook a subclass brings.
        const MethodTable& classMethodTable = classInfo->methodTable;
        if (object->inherits(classInfo)
            && methodTable->put == classMethodTable.put
            && methodTable->putByIndex == classMethodTable.putByIndex
            && methodTable->deleteProperty == classMethodTable.deleteProperty
            && methodTable->deletePropertyByIndex == classMethodTable.deletePropertyByIndex
            && methodTable->defineOwnProperty == classMethodTable.defineOwnProperty
            && methodTable->setPrototype == classMethodTable.setPrototype
            && methodTable->preventExtensions == classMethodTable.preventExtensions)
            return true;
    }
    return false;
}

bool JSObject::hasImmutableProperties() const
{
    if (type() == GlobalProxyType)
        return uncheckedDowncast<JSGlobalProxy>(this)->target()->hasImmutableProperties();
    return structure()->hasImmutableProperties();
}

// Copy-on-write storage is the one kind every tier reads in place and no tier stores into in place. It is a cell of its own with
// room for elements only, and its users expect no holes. Null: this object's elements cannot go there.
JSCellButterfly* JSObject::tryCreateCopyOnWriteButterfly(VM& vm, JSObject* object)
{
    if (!isJSArray(object) || object->structure()->outOfLineCapacity() || object->structure()->hijacksIndexingHeader())
        return nullptr;
    IndexingType type = object->indexingType();
    Butterfly* butterfly = object->butterfly();
    unsigned length = butterfly->publicLength();
    IndexingType copyOnWriteType;
    if (hasInt32(type))
        copyOnWriteType = CopyOnWriteArrayWithInt32;
    else if (hasDouble(type))
        copyOnWriteType = CopyOnWriteArrayWithDouble;
    else if (hasContiguous(type))
        copyOnWriteType = CopyOnWriteArrayWithContiguous;
    else
        return nullptr;
    for (unsigned i = 0; i < length; ++i) {
        if (hasDouble(type)) {
            double value = butterfly->contiguousDouble().at(object, i);
            if (value != value)
                return nullptr;
        } else if (!butterfly->contiguous().at(object, i).get())
            return nullptr;
    }
    JSCellButterfly* result = JSCellButterfly::tryCreate(vm, copyOnWriteType, length);
    if (!result)
        return nullptr;
    // (Allocating can run the collector, which leaves the object's own storage where it is.)
    butterfly = object->butterfly();
    for (unsigned i = 0; i < length; ++i) {
        if (hasDouble(type))
            result->setIndex(vm, i, jsDoubleNumber(butterfly->contiguousDouble().at(object, i)));
        else
            result->setIndex(vm, i, butterfly->contiguous().at(object, i).get());
    }
    return result;
}

bool JSObject::makePropertiesImmutable(VM& vm)
{
    if (structure()->hasImmutableProperties())
        return true;
    // `globalThis` may be the proxy in front of the global object: act on the object behind it.
    if (type() == GlobalProxyType)
        return uncheckedDowncast<JSGlobalProxy>(this)->target()->makePropertiesImmutable(vm);
    if (!canMakePropertiesImmutable(this))
        return false;
    // The global object keeps top-level `var` and function declarations in its symbol table, and compiled code writes those slots
    // directly, past every property check. Make each one read-only and fire the watchpoint that makes such code look again: the
    // same two steps JSGlobalObject::defineOwnProperty takes when a script freezes the global.
    if (auto* global = dynamicDowncast<JSGlobalObject>(this)) {
        bool changed = false;
        {
            SymbolTable* symbolTable = global->symbolTable();
            ConcurrentJSLocker locker(symbolTable->m_lock);
            for (auto iter = symbolTable->begin(locker), end = symbolTable->end(locker); iter != end; ++iter) {
                if (!iter->value.isReadOnly()) {
                    iter->value.setReadOnly();
                    changed = true;
                }
            }
        }
        if (changed)
            global->varReadOnlyWatchpointSet().fireAll(vm, "The global object's properties were made immutable");
    }
    // The JIT and the quick C++ paths write Int32/Double/Contiguous/ArrayStorage elements in place, slow-put storage included
    // (it only diverts stores to holes). There are two kinds of storage they do not write in place, so that each indexed store,
    // delete and length change reaches a C++ path, which refuses: copy-on-write storage, which every tier still reads in place,
    // and the sparse map of dictionary indexing mode. A JSArray's Int32, Double or Contiguous elements go to the first if they have
    // no holes (an array literal's are there already), elements of any other kind to the second. Objects with no indexed storage (every intrinsic prototype) are
    // untouched, so Array.prototype keeps its blank indexing.
    JSCellButterfly* copyOnWriteButterfly = nullptr;
    if (hasIndexedProperties(indexingType())) {
        bool willBeCopyOnWrite = false;
        if (Options::useCopyOnWriteArraysForImmutableProperties()) {
            if (isCopyOnWrite(indexingMode()))
                willBeCopyOnWrite = true;
            else {
                copyOnWriteButterfly = tryCreateCopyOnWriteButterfly(vm, this);
                willBeCopyOnWrite = copyOnWriteButterfly;
            }
        }
        if (!willBeCopyOnWrite)
            enterDictionaryIndexingMode(vm);
    }
    // Compiled code tests RegExpObject's own lastIndex-writable flag, not the Structure: make the two agree, and tell the realm, as
    // RegExpObject::defineOwnProperty() does, so that code which folded a search on a constant RegExp is not relied on.
    if (auto* regExpObject = dynamicDowncast<RegExpObject>(this)) {
        regExpObject->setLastIndexIsNotWritable();
        regExpObject->realm()->regExpLastIndexWritableWatchpointSet().fireAll(vm, "RegExp lastIndex was made non-writable");
    }
    StructureID oldStructureID = structureID();
    Structure* oldStructure = oldStructureID.decode();
    // Deferred, so adaptive watchpoints on this object see the new structure and re-install instead of firing their sets.
    DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
    Structure* newStructure = Structure::makePropertiesImmutableTransition(vm, oldStructure, &deferred);
    RELEASE_ASSERT(isCopyOnWrite(newStructure->indexingMode()) == (copyOnWriteButterfly || isCopyOnWrite(oldStructure->indexingMode())));
    if (copyOnWriteButterfly)
        nukeStructureAndSetButterfly(vm, oldStructureID, copyOnWriteButterfly->toButterfly());
    setStructure(vm, newStructure);
    if (mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change, this);
    return true;
}

bool JSObject::makePropertiesImmutable(VM& vm, ImmutablePropertyNames& requestedNames)
{
    Structure* oldStructure = structure();
    if (oldStructure->hasImmutableProperties())
        return true;
    if (type() == GlobalProxyType)
        return uncheckedDowncast<JSGlobalProxy>(this)->target()->makePropertiesImmutable(vm, requestedNames);
    if (oldStructure->isCompiledStructure() || !canMakePropertiesImmutable(this))
        return false;
    if (requestedNames.isEmpty())
        return true;
    // Every tier stores into indexed storage in place, so only an object that has none can keep numeric keys out.
    if (requestedNames.parts().contains(ImmutablePropertyNames::Part::NumericKeys) && hasIndexedProperties(indexingType()))
        return false;

    Ref<ImmutablePropertyNames> names = requestedNames;
    if (ImmutablePropertyNames* existingNames = oldStructure->listedImmutablePropertyNames()) {
        if (existingNames->containsAll(requestedNames))
            return true;
        names = ImmutablePropertyNames::createUnion(*existingNames, requestedNames);
    }

    if (auto* global = dynamicDowncast<JSGlobalObject>(this)) {
        bool changed = false;
        {
            SymbolTable* symbolTable = global->symbolTable();
            ConcurrentJSLocker locker(symbolTable->m_lock);
            for (auto iter = symbolTable->begin(locker), end = symbolTable->end(locker); iter != end; ++iter) {
                if (!iter->value.isReadOnly() && names->contains(iter->key.get())) {
                    iter->value.setReadOnly();
                    changed = true;
                }
            }
        }
        if (changed)
            global->varReadOnlyWatchpointSet().fireAll(vm, "Some of the global object's properties were made immutable");
    }

    DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
    setStructure(vm, Structure::makeListedPropertiesImmutableTransition(vm, oldStructure, names.get(), &deferred));
    if (mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change, this);
    return true;
}

static void putAddedPropertiesAndSetStructure(VM& vm, JSObject* object, Structure* newStructure, std::span<const EncodedJSValue> values)
{
    Structure* oldStructure = object->structure();
    unsigned firstPropertyNumber = numberOfSlotsForMaxOffset(oldStructure->maxOffset(), oldStructure->inlineCapacity());
    ASSERT(newStructure->inlineCapacity() == oldStructure->inlineCapacity());
    ASSERT(numberOfSlotsForMaxOffset(newStructure->maxOffset(), newStructure->inlineCapacity()) == firstPropertyNumber + values.size());

    object->growOutOfLineStorageForTransition(vm, newStructure);
    for (unsigned i = 0; i < values.size(); ++i)
        object->putDirectOffset(vm, offsetForPropertyNumber(firstPropertyNumber + i, oldStructure->inlineCapacity()), JSValue::decode(values[i]));
    object->setStructure(vm, newStructure);
}

bool JSObject::tryPutDirectPropertyRun(VM& vm, const PropertyRun& run, std::span<const EncodedJSValue> values)
{
    ASSERT(values.size() == run.size());
    if (objectTypesWithoutButterfly.contains(type())) [[unlikely]]
        return false;
    Structure* oldStructure = structure();
    std::optional<DeferredStructureTransitionWatchpointFire> deferred;
    Structure* newStructure = Structure::addPropertyRunTransitionToExistingStructure(oldStructure, run);
    if (!newStructure) {
        deferred.emplace(vm, oldStructure);
        newStructure = Structure::addPropertyRunTransition(vm, oldStructure, run, &*deferred);
        if (!newStructure)
            return false;
    }

    putAddedPropertiesAndSetStructure(vm, this, newStructure, values);

    if (mayBePrototype()) [[unlikely]] {
        Vector<UniquedStringImpl*, 16> names(run.size(), [&](size_t index) {
            return run.names()[index].get();
        });
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add, this, names.span());
    }
    return true;
}

bool JSObject::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    ASSERT(!object->structure()->isCompiledStructure());
    VM& vm = globalObject->vm();
    if (!object->isStructureExtensible()) {
        // We've already set the internal [[PreventExtensions]] field to false.
        // We don't call the methodTable isExtensible here because it's not defined
        // that way in the specification. We are just doing an optimization here.
        return true;
    }

    object->enterDictionaryIndexingMode(vm);
    {
        Structure* oldStructure = object->structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        object->setStructure(vm, Structure::preventExtensionsTransition(vm, oldStructure, &deferred));
    }
    return true;
}

bool JSObject::isExtensible(JSObject* obj, JSGlobalObject*)
{
    return obj->isStructureExtensible();
}

bool JSObject::isExtensible(JSGlobalObject* globalObject)
{ 
    return methodTable()->isExtensible(this, globalObject);
}

void JSObject::reifyAllStaticProperties(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    ASSERT(!staticPropertiesReified());

    // If this object's ClassInfo has no static properties, then nothing to reify!
    // We can safely set the flag to avoid the expensive check again in the future.
    if (!TypeInfo::hasStaticPropertyTable(inlineTypeFlags())) {
        structure()->setStaticPropertiesReified(true);
        return;
    }

    if (!structure()->isDictionary())
        convertToDictionary(vm);

    // A PropertyCallback builder can enter JS; defer termination (like
    // LazyProperty::callFunc) so it can't return with one pending. No
    // ThrowScope here: JSObject::deleteProperty reaches this without one.
    DeferTerminationForAWhile deferScope(vm);
    for (const ClassInfo* info = classInfo(); info; info = info->parentClass) {
        const HashTable* hashTable = info->staticPropHashTable;
        if (!hashTable)
            continue;

        for (auto& value : *hashTable) {
            unsigned attributes;
            auto key = Identifier::fromString(vm, value.m_key);
            PropertyOffset offset = getDirectOffset(vm, key, attributes);
            if (!isValidOffset(offset)) {
#if ENABLE(AOT)
                // Compiled code may hold the object already. The property is that object.
                JSObject* madeAhead = vm.hasCompiledCode() && !(value.attributes() & PropertyAttribute::Accessor) ? AOT::RuntimeHooks::takeStaticPropertyMadeAhead(this, key.impl()) : nullptr;
                if (madeAhead) [[unlikely]] {
                    AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
                    putDirect(vm, key, madeAhead, attributesForStructure(value.attributes()));
                } else
#endif
                    reifyStaticProperty(vm, hashTable->classForThis, key, value, *this);
                // Leave the rest lazy on throw; the caller propagates.
                if (vm.exceptionForInspection()) [[unlikely]] {
#if ENABLE(AOT)
                    JSValue standIn = vm.hasCompiledCode() ? AOT::takeStandInForMemberLeftOut(globalObject, key.string()) : JSValue();
                    if (!standIn)
                        return;
                    AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
                    putDirect(vm, key, standIn, attributesForStructure(value.attributes()));
#else
                    return;
#endif
                }
#if ENABLE(AOT)
                if (vm.hasCompiledCode() && !(value.attributes() & PropertyAttribute::Accessor)) [[unlikely]]
                    AOT::RuntimeHooks::didCreateStaticProperty(this, key.impl(), getDirect(vm, key));
#endif
            }
        }
    }

    structure()->setStaticPropertiesReified(true);
}

NEVER_INLINE void JSObject::fillGetterPropertySlot(VM&, PropertySlot& slot, JSCell* getterSetter, unsigned attributes, PropertyOffset offset)
{
    slot.setCacheableGetterSlot(this, attributes, uncheckedDowncast<GetterSetter>(getterSetter), offset);
}

static bool putIndexedDescriptor(JSGlobalObject* globalObject, SparseArrayValueMap* map, SparseArrayEntry* entryInMap, const PropertyDescriptor& descriptor, PropertyDescriptor& oldDescriptor)
{
    VM& vm = globalObject->vm();

    if (descriptor.isDataDescriptor()) {
        unsigned attributes = descriptor.attributesOverridingCurrent(oldDescriptor) & ~PropertyAttribute::Accessor;
        if (descriptor.value())
            entryInMap->forceSet(vm, map, descriptor.value(), attributes);
        else if (oldDescriptor.isAccessorDescriptor())
            entryInMap->forceSet(vm, map, jsUndefined(), attributes);
        else
            entryInMap->forceSet(map, attributes);
        return true;
    }

    if (descriptor.isAccessorDescriptor()) {
        JSObject* getter = nullptr;
        if (descriptor.getterPresent())
            getter = descriptor.getterObject();
        else if (oldDescriptor.isAccessorDescriptor())
            getter = oldDescriptor.getterObject();
        JSObject* setter = nullptr;
        if (descriptor.setterPresent())
            setter = descriptor.setterObject();
        else if (oldDescriptor.isAccessorDescriptor())
            setter = oldDescriptor.setterObject();

        GetterSetter* accessor = GetterSetter::create(vm, globalObject, getter, setter);
        entryInMap->forceSet(vm, map, accessor, descriptor.attributesOverridingCurrent(oldDescriptor) & ~PropertyAttribute::ReadOnly);
        return true;
    }

    ASSERT(descriptor.isGenericDescriptor());
    entryInMap->forceSet(map, descriptor.attributesOverridingCurrent(oldDescriptor));
    return true;
}

ALWAYS_INLINE static bool canDoFastPutDirectIndex(JSObject* object)
{
    if (TypeInfo::isArgumentsType(object->type()))
        return true;

    if (object->inSparseIndexingMode())
        return false;

    return (isJSArray(object) && !isCopyOnWrite(object->indexingMode()))
        || is<JSFinalObject>(object);
}

// https://tc39.es/ecma262/#sec-ordinarydefineownproperty
bool JSObject::defineOwnIndexedProperty(JSGlobalObject* globalObject, unsigned index, const PropertyDescriptor& descriptor, bool throwException)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // The descriptor is validated against the current one, which defineOwnNonIndexProperty() does. One that only restricts the element comes back here to be applied.
    if (structure()->hasImmutableIndexedProperties() && !vm.allowLazyMaterializationOfImmutablePropertiesCount) [[unlikely]]
        RELEASE_AND_RETURN(scope, defineOwnNonIndexProperty(globalObject, Identifier::from(vm, index), descriptor, throwException));

    ASSERT(index <= MAX_ARRAY_INDEX);

    ensureWritable(vm);

    if (!inSparseIndexingMode()) {
        const PropertyDescriptor emptyAttributesDescriptor(jsUndefined(), static_cast<unsigned>(PropertyAttribute::None));
        ASSERT(emptyAttributesDescriptor.attributes() == static_cast<unsigned>(PropertyAttribute::None));

#if ASSERT_ENABLED
        if (canGetIndexQuickly(index) && canDoFastPutDirectIndex(this)) {
            DeferTermination deferScope(vm);
            PropertyDescriptor currentDescriptor;
            bool found = getOwnPropertyDescriptor(globalObject, Identifier::from(vm, index), currentDescriptor);
            scope.assertNoException();
            if (found)
                ASSERT(currentDescriptor.attributes() == emptyAttributesDescriptor.attributes());
        }
#endif
        // Fast case: we're putting a regular property to a regular array
        if (descriptor.value()
            && (!descriptor.attributes() || (canGetIndexQuickly(index) && !descriptor.attributesOverridingCurrent(emptyAttributesDescriptor)))
            && canDoFastPutDirectIndex(this)) {
            ASSERT(!descriptor.isAccessorDescriptor());
            RELEASE_AND_RETURN(scope, putDirectIndex(globalObject, index, descriptor.value(), 0, throwException ? PutDirectIndexShouldThrow : PutDirectIndexShouldNotThrow));
        }
        
        ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm);
    }

    if (descriptor.attributes() & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor))
        notifyPresenceOfIndexedAccessors(vm);

    SparseArrayValueMap* map = this->butterfly()->arrayStorage()->m_sparseMap.get();
    RELEASE_ASSERT(map);
    
    // 1. Let current be the result of calling the [[GetOwnProperty]] internal method of O with property name P.
    SparseArrayValueMap::AddResult result = map->add(this, index);
    SparseArrayEntry* entryInMap = &*result.iterator;

    // 2. Let extensible be the value of the [[Extensible]] internal property of O.
    // 3. If current is undefined and extensible is false, then Reject.
    // 4. If current is undefined and extensible is true, then
    if (result.isNewEntry) {
        if (!isStructureExtensible()) {
            map->remove(static_cast<SparseArrayValueMap::const_iterator>(result.iterator));
            return typeError(globalObject, scope, throwException, NonExtensibleObjectPropertyDefineError);
        }

        // 4.a. If IsGenericDescriptor(Desc) or IsDataDescriptor(Desc) is true, then create an own data property
        // named P of object O whose [[Value]], [[Writable]], [[Enumerable]] and [[Configurable]] attribute values
        // are described by Desc. If the value of an attribute field of Desc is absent, the attribute of the newly
        // created property is set to its default value.
        // 4.b. Else, Desc must be an accessor Property Descriptor so, create an own accessor property named P of
        // object O whose [[Get]], [[Set]], [[Enumerable]] and [[Configurable]] attribute values are described by
        // Desc. If the value of an attribute field of Desc is absent, the attribute of the newly created property
        // is set to its default value.
        // 4.c. Return true.

        PropertyDescriptor defaults(jsUndefined(), PropertyAttribute::DontDelete | PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
        putIndexedDescriptor(globalObject, map, entryInMap, descriptor, defaults);
        auto* butterfly = this->butterfly();
        if (index >= butterfly->arrayStorage()->length())
            butterfly->arrayStorage()->setLength(index + 1);
        return true;
    }

    // 5. Return true, if every field in Desc is absent.
    // 6. Return true, if every field in Desc also occurs in current and the value of every field in Desc is the same value as the corresponding field in current when compared using the SameValue algorithm (9.12).
    PropertyDescriptor current;
    entryInMap->get(current);
    bool isEmptyOrEqual = descriptor.isEmpty() || descriptor.equalTo(globalObject, current);
    RETURN_IF_EXCEPTION(scope, false);
    if (isEmptyOrEqual)
        return true;

    // 7. If the [[Configurable]] field of current is false then
    if (!current.configurable()) {
        // 7.a. Reject, if the [[Configurable]] field of Desc is true.
        if (descriptor.configurablePresent() && descriptor.configurable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeConfigurabilityError);
        // 7.b. Reject, if the [[Enumerable]] field of Desc is present and the [[Enumerable]] fields of current and Desc are the Boolean negation of each other.
        if (descriptor.enumerablePresent() && current.enumerable() != descriptor.enumerable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeEnumerabilityError);
    }

    // 8. If IsGenericDescriptor(Desc) is true, then no further validation is required.
    if (!descriptor.isGenericDescriptor()) {
        // 9. Else, if IsDataDescriptor(current) and IsDataDescriptor(Desc) have different results, then
        if (current.isDataDescriptor() != descriptor.isDataDescriptor()) {
            // 9.a. Reject, if the [[Configurable]] field of current is false.
            if (!current.configurable())
                return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeAccessMechanismError);
            // 9.b. If IsDataDescriptor(current) is true, then convert the property named P of object O from a
            // data property to an accessor property. Preserve the existing values of the converted property's
            // [[Configurable]] and [[Enumerable]] attributes and set the rest of the property's attributes to
            // their default values.
            // 9.c. Else, convert the property named P of object O from an accessor property to a data property.
            // Preserve the existing values of the converted property's [[Configurable]] and [[Enumerable]]
            // attributes and set the rest of the property's attributes to their default values.
        } else if (current.isDataDescriptor() && descriptor.isDataDescriptor()) {
            // 10. Else, if IsDataDescriptor(current) and IsDataDescriptor(Desc) are both true, then
            // 10.a. If the [[Configurable]] field of current is false, then
            if (!current.configurable() && !current.writable()) {
                // 10.a.i. Reject, if the [[Writable]] field of current is false and the [[Writable]] field of Desc is true.
                if (descriptor.writable())
                    return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeWritabilityError);
                // 10.a.ii. If the [[Writable]] field of current is false, then
                // 10.a.ii.1. Reject, if the [[Value]] field of Desc is present and SameValue(Desc.[[Value]], current.[[Value]]) is false.
                if (descriptor.value()) {
                    bool isSame = sameValue(globalObject, descriptor.value(), current.value());
                    RETURN_IF_EXCEPTION(scope, false);
                    if (!isSame)
                        return typeError(globalObject, scope, throwException, ReadonlyPropertyChangeError);
                }
            }
            // 10.b. else, the [[Configurable]] field of current is true, so any change is acceptable.
        } else {
            ASSERT(current.isAccessorDescriptor() && current.getterPresent() && current.setterPresent());
            // 11. Else, IsAccessorDescriptor(current) and IsAccessorDescriptor(Desc) are both true so, if the [[Configurable]] field of current is false, then
            if (!current.configurable()) {
                // 11.i. Reject, if the [[Set]] field of Desc is present and SameValue(Desc.[[Set]], current.[[Set]]) is false.
                if (descriptor.setterPresent() && descriptor.setter() != current.setter())
                    return typeError(globalObject, scope, throwException, "Attempting to change the setter of an unconfigurable property."_s);
                // 11.ii. Reject, if the [[Get]] field of Desc is present and SameValue(Desc.[[Get]], current.[[Get]]) is false.
                if (descriptor.getterPresent() && descriptor.getter() != current.getter())
                    return typeError(globalObject, scope, throwException, "Attempting to change the getter of an unconfigurable property."_s);
            }
        }
    }

    // 12. For each attribute field of Desc that is present, set the correspondingly named attribute of the property named P of object O to the value of the field.
    putIndexedDescriptor(globalObject, map, entryInMap, descriptor, current);
    // 13. Return true.
    return true;
}

SparseArrayValueMap* JSObject::allocateSparseIndexMap(VM& vm)
{
    SparseArrayValueMap* result = SparseArrayValueMap::create(vm);
    arrayStorage()->m_sparseMap.set(vm, this, result);
    return result;
}

void JSObject::deallocateSparseIndexMap()
{
    if (ArrayStorage* arrayStorage = arrayStorageOrNull())
        arrayStorage->m_sparseMap.clear();
}

bool JSObject::attemptToInterceptPutByIndexOnHoleForPrototype(JSGlobalObject* globalObject, JSValue thisValue, unsigned i, JSValue value, bool shouldThrow, bool& putResult)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    for (JSObject* current = this; ;) {
        // This has the same behavior with respect to prototypes as JSObject::put(). It only
        // allows a prototype to intercept a put if (a) the prototype declares the property
        // we're after rather than intercepting it via an override of JSObject::put(), and
        // (b) that property is declared as ReadOnly or Accessor.
        
        ArrayStorage* storage = current->arrayStorageOrNull();
#if ENABLE(AOT)
        if (AOT::PropertyWalk::isCompiledObject(current)) [[unlikely]] {
            if (JSObject* holder = AOT::PropertyWalk::objectHoldingIndexedProperties(current))
                storage = holder->arrayStorageOrNull();
        }
#endif
        if (storage && storage->m_sparseMap) {
            SparseArrayValueMap::iterator iter = storage->m_sparseMap->find(i);
            if (iter != storage->m_sparseMap->notFound() && (iter->attributes() & (PropertyAttribute::Accessor | PropertyAttribute::ReadOnly))) {
                scope.release();
                putResult = SparseArrayValueMap::entryFor(iter).put(globalObject, thisValue, storage->m_sparseMap.get(), value, shouldThrow);
                return true;
            }
        }

        if (current->hasFrozenElementsInPlace() && current->canGetIndexQuickly(i)) [[unlikely]] {
            putResult = typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
            return true;
        }

        if (current->type() == ProxyObjectType) {
            scope.release();
            auto* proxy = uncheckedDowncast<ProxyObject>(current);
            putResult = proxy->putByIndexCommon(globalObject, thisValue, i, value, shouldThrow);
            return true;
        }

        if (isTypedArrayType(current->type())) {
            auto* typedArray = uncheckedDowncast<JSArrayBufferView>(current);
            if (typedArray->isOutOfBounds() || i >= typedArray->length()) {
                putResult = true;
                return true;
            }
            return false;
        }

        JSValue prototypeValue = current->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (prototypeValue.isNull())
            return false;
        
        current = asObject(prototypeValue);
    }
}

bool JSObject::attemptToInterceptPutByIndexOnHole(JSGlobalObject* globalObject, unsigned i, JSValue value, bool shouldThrow, bool& putResult)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue prototypeValue = getPrototype(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (prototypeValue.isNull())
        return false;
    
    RELEASE_AND_RETURN(scope, asObject(prototypeValue)->attemptToInterceptPutByIndexOnHoleForPrototype(globalObject, this, i, value, shouldThrow, putResult));
}

template<IndexingType indexingShape>
bool JSObject::putByIndexBeyondVectorLengthWithoutAttributes(JSGlobalObject* globalObject, unsigned i, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    RELEASE_ASSERT_WITH_SECURITY_IMPLICATION(!isCopyOnWrite(indexingMode()));
    ASSERT((indexingType() & IndexingShapeMask) == indexingShape);
    ASSERT(!indexingShouldBeSparse());

    auto* butterfly = this->butterfly();

    // For us to get here, the index is either greater than the public length, or greater than
    // or equal to the vector length.
    ASSERT(i >= butterfly->vectorLength());

    ArrayStoragePolicy policy = vm.arrayStoragePolicy();
    unsigned numberOfValues = countsValuesToStoreBeyondVector(policy, i) ? countElements<indexingShape>(butterfly) : 0;
    if (!storesBeyondVectorInDenseStorage(policy, i, butterfly->vectorLength(), numberOfValues)) {
        ASSERT(i <= MAX_ARRAY_INDEX);
        ArrayStorage* storage = ensureArrayStorageSlow(vm);
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, false, storage));
    }

    if (!ensureLength(vm, i + 1)) {
        throwOutOfMemoryError(globalObject, scope);
        return false;
    }
    butterfly = this->butterfly();

    RELEASE_ASSERT(i < butterfly->vectorLength());
    switch (indexingShape) {
    case Int32Shape:
        ASSERT(value.isInt32());
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(value);
        return true;
        
    case DoubleShape: {
        ASSERT(Options::allowDoubleShape());
        ASSERT(value.isNumber());
        double valueAsDouble = value.asNumber();
        ASSERT(valueAsDouble == valueAsDouble);
        butterfly->contiguousDouble().at(this, i) = valueAsDouble;
        return true;
    }
        
    case ContiguousShape:
        butterfly->contiguous().at(this, i).set(vm, this, value);
        return true;
        
    default:
        CRASH();
        return false;
    }
}

// Explicit instantiations needed by JSArray.cpp.
template bool JSObject::putByIndexBeyondVectorLengthWithoutAttributes<Int32Shape>(JSGlobalObject*, unsigned, JSValue);
template bool JSObject::putByIndexBeyondVectorLengthWithoutAttributes<DoubleShape>(JSGlobalObject*, unsigned, JSValue);
template bool JSObject::putByIndexBeyondVectorLengthWithoutAttributes<ContiguousShape>(JSGlobalObject*, unsigned, JSValue);

bool JSObject::putByIndexBeyondVectorLengthWithArrayStorage(JSGlobalObject* globalObject, unsigned i, JSValue value, bool shouldThrow, ArrayStorage* storage)
{
    VM& vm = globalObject->vm();
    // We're transitioning between states here, if a termination comes in we could leave the object
    // in an inconsistent state. We could still be in the middle a GC during termination so we could
    // try to mark this object and crash. It's much easier to just not think about it.
    DeferTerminationForAWhile noTermination(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(!isCopyOnWrite(indexingMode()));
    // i should be a valid array index that is outside of the current vector.
    ASSERT(i <= MAX_ARRAY_INDEX);
    ASSERT(i >= storage->vectorLength());
    
    SparseArrayValueMap* map = storage->m_sparseMap.get();
    
    // First, handle cases where we don't currently have a sparse map.
    if (!map) [[likely]] {
        // If the array is not extensible, we should have entered dictionary mode, and created the sparse map.
        ASSERT(isStructureExtensible());
    
        // Update m_length if necessary.
        if (i >= storage->length())
            storage->setLength(i + 1);

        // Check that it is sensible to still be using a vector, and then try to grow the vector.
        if (!indexIsSufficientlyBeyondLengthForSparseMap(i, storage->vectorLength())
            && isDenseEnoughForVector(i, storage->m_numValuesInVector)
            && increaseVectorLength(vm, i + 1)) [[likely]] {
            // success! - reread m_storage since it has likely been reallocated, and store to the vector.
            storage = arrayStorage();
            storage->m_vector[i].set(vm, this, value);
            ++storage->m_numValuesInVector;
            if (returnsFromArrayStorageWhenDense(vm.arrayStoragePolicy()))
                tryConvertArrayStorageToContiguous(vm);
            return true;
        }
        // We don't want to, or can't use a vector to hold this property - allocate a sparse map & add the value.
        map = allocateSparseIndexMap(vm);
        RELEASE_AND_RETURN(scope, map->putEntry(globalObject, this, i, value, shouldThrow));
    }

    // Update m_length if necessary.
    unsigned length = storage->length();
    if (i >= length) {
        // Prohibit growing the array if length is not writable.
        if (map->lengthIsReadOnly() || !isStructureExtensible())
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
        length = i + 1;
        storage->setLength(length);
    }

    // We are currently using a map - check whether we still want to be doing so.
    // We will continue  to use a sparse map if SparseMode is set, a vector would be too sparse, or if allocation fails.
    unsigned numValuesInArray = storage->m_numValuesInVector + map->size();
    if (map->sparseMode() || !isDenseEnoughForVector(length, numValuesInArray) || !increaseVectorLength(vm, length))
        RELEASE_AND_RETURN(scope, map->putEntry(globalObject, this, i, value, shouldThrow));

    // Reread m_storage after increaseVectorLength, update m_numValuesInVector.
    storage = arrayStorage();
    storage->m_numValuesInVector = numValuesInArray;

    // Copy all values from the map into the vector, and delete the map.
    WriteBarrier<Unknown>* vector = storage->m_vector;
    SparseArrayValueMap::const_iterator end = map->end();
    for (SparseArrayValueMap::const_iterator it = map->begin(); it != end; ++it)
        vector[it->index()].set(vm, this, it->getNonSparseMode());
    deallocateSparseIndexMap();

    // Store the new property into the vector.
    WriteBarrier<Unknown>& valueSlot = vector[i];
    if (!valueSlot)
        ++storage->m_numValuesInVector;
    valueSlot.set(vm, this, value);
    if (returnsFromArrayStorageWhenDense(vm.arrayStoragePolicy()))
        tryConvertArrayStorageToContiguous(vm);
    return true;
}

bool JSObject::tryConvertArrayStorageToContiguous(VM& vm)
{
    Structure* structure = this->structure();
    if (structure->indexingType() != ArrayWithArrayStorage || structure->outOfLineCapacity() || structure->hasImmutableProperties() || !structure->isStructureExtensible() || needsSlowPutIndexing())
        return false;
    ArrayStorage* storage = butterfly()->arrayStorage();
    SparseArrayValueMap* map = storage->m_sparseMap.get();
    unsigned length = storage->length();
    if (length > MAX_STORAGE_VECTOR_LENGTH || (map && (map->sparseMode() || map->lengthIsReadOnly())))
        return false;
    if (!isDenseEnoughForVector(length, storage->m_numValuesInVector + (map ? map->size() : 0)))
        return false;

    DeferGC deferGC(vm);
    unsigned vectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), length);
    Butterfly* newButterfly = Butterfly::tryCreateUninitialized(vm, this, 0, 0, true, vectorLength * sizeof(EncodedJSValue));
    if (!newButterfly)
        return false;
    newButterfly->setVectorLength(vectorLength);
    newButterfly->setPublicLength(length);
    WriteBarrier<Unknown>* elements = newButterfly->contiguous().data();
    unsigned numberInVector = std::min(storage->vectorLength(), length);
    for (unsigned i = 0; i < numberInVector; ++i)
        elements[i].setWithoutWriteBarrier(storage->m_vector[i].get());
    for (unsigned i = numberInVector; i < vectorLength; ++i)
        elements[i].clear();
    if (map) {
        for (auto& entry : *map)
            elements[entry.index()].setWithoutWriteBarrier(entry.getNonSparseMode());
    }

    DeferredStructureTransitionWatchpointFire deferred(vm, structure);
    Structure* newStructure = Structure::nonPropertyTransition(vm, structure, TransitionKind::AllocateContiguous, &deferred);
    {
        Locker locker { cellLock() };
        nukeStructureAndSetButterfly(vm, structure->id(), newButterfly);
        setStructure(vm, newStructure);
    }
    vm.writeBarrier(this);
    return true;
}

bool JSObject::putByIndexBeyondVectorLength(JSGlobalObject* globalObject, unsigned i, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    RELEASE_ASSERT_WITH_SECURITY_IMPLICATION(!isCopyOnWrite(indexingMode()));

    // i should be a valid array index that is outside of the current vector.
    ASSERT(i <= MAX_ARRAY_INDEX);
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES: {
        if (indexingShouldBeSparse()) {
            auto* arrayStorage = ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm);
            if (!hasSlowPutArrayStorage(indexingType())) [[likely]]
                RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, shouldThrow, arrayStorage));
        } else if (!storesBeyondVectorInDenseStorage(vm.arrayStoragePolicy(), i, 0, 0)) {
            auto* arrayStorage = createArrayStorage(vm, 0, 0);
            if (!hasSlowPutArrayStorage(indexingType())) [[likely]]
                RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, shouldThrow, arrayStorage));
        } else if (needsSlowPutIndexing()) [[unlikely]] {
            // Convert the indexing type to the SlowPutArrayStorage and retry.
            createArrayStorage(vm, i + 1, getNewVectorLength(0, 0, 0, i + 1));
        } else {
            createInitialForValueAndSet(vm, i, value);
            return true;
        }
        // Fallback with SlowPutArrayStorage.
        RELEASE_AND_RETURN(scope, putByIndex(this, globalObject, i, value, shouldThrow));
    }
        
    case ALL_UNDECIDED_INDEXING_TYPES: {
        CRASH();
        break;
    }
        
    case ALL_INT32_INDEXING_TYPES:
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithoutAttributes<Int32Shape>(globalObject, i, value));
        
    case ALL_DOUBLE_INDEXING_TYPES:
        ASSERT(Options::allowDoubleShape());
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithoutAttributes<DoubleShape>(globalObject, i, value));
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithoutAttributes<ContiguousShape>(globalObject, i, value));
        
    case NonArrayWithSlowPutArrayStorage:
    case ArrayWithSlowPutArrayStorage: {
        // No own property present in the vector, but there might be in the sparse map!
        SparseArrayValueMap* map = arrayStorage()->m_sparseMap.get();
        bool putResult = false;
        if (!(map && map->contains(i))) {
            bool result = attemptToInterceptPutByIndexOnHole(globalObject, i, value, shouldThrow, putResult);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return putResult;
        }
        [[fallthrough]];
    }

    case NonArrayWithArrayStorage:
    case ArrayWithArrayStorage:
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, shouldThrow, arrayStorage()));
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return false;
}

bool JSObject::putDirectIndexBeyondVectorLengthWithArrayStorage(JSGlobalObject* globalObject, unsigned i, JSValue value, unsigned attributes, PutDirectIndexMode mode, ArrayStorage* storage)
{
    VM& vm = globalObject->vm();
    // We're transitioning between states here, if a termination comes in we could leave the object
    // in an inconsistent state. We could still be in the middle a GC during termination so we could
    // try to mark this object and crash. It's much easier to just not think about it.
    DeferTerminationForAWhile noTermination(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);
    
    // i should be a valid array index that is outside of the current vector.
    ASSERT(hasAnyArrayStorage(indexingType()));
    ASSERT(arrayStorage() == storage);
    ASSERT(i >= storage->vectorLength() || attributes);
    ASSERT(i <= MAX_ARRAY_INDEX);

    SparseArrayValueMap* map = storage->m_sparseMap.get();

    // First, handle cases where we don't currently have a sparse map.
    if (!map) [[likely]] {
        // If the array is not extensible, we should have entered dictionary mode, and created the spare map.
        ASSERT(isStructureExtensible());
    
        // Update m_length if necessary.
        if (i >= storage->length())
            storage->setLength(i + 1);

        // Check that it is sensible to still be using a vector, and then try to grow the vector.
        if (!attributes
            && (isDenseEnoughForVector(i, storage->m_numValuesInVector))
            && !indexIsSufficientlyBeyondLengthForSparseMap(i, storage->vectorLength()))  [[likely]] {
            if (increaseVectorLength(vm, i + 1)) {
                // success! - reread m_storage since it has likely been reallocated, and store to the vector.
                storage = arrayStorage();
                storage->m_vector[i].set(vm, this, value);
                ++storage->m_numValuesInVector;
                if (returnsFromArrayStorageWhenDense(vm.arrayStoragePolicy()))
                    tryConvertArrayStorageToContiguous(vm);
                return true;
            }
        }
        // We don't want to, or can't use a vector to hold this property - allocate a sparse map & add the value.
        map = allocateSparseIndexMap(vm);
        RELEASE_AND_RETURN(scope, map->putDirect(globalObject, this, i, value, attributes, mode));
    }

    // Update m_length if necessary.
    unsigned length = storage->length();
    if (i >= length) {
        if (mode != PutDirectIndexLikePutDirect) {
            // Prohibit growing the array if length is not writable.
            if (map->lengthIsReadOnly())
                return typeError(globalObject, scope, mode == PutDirectIndexShouldThrow, ReadonlyPropertyWriteError);
            if (!isStructureExtensible())
                return typeError(globalObject, scope, mode == PutDirectIndexShouldThrow, NonExtensibleObjectPropertyDefineError);
        }
        length = i + 1;
        storage->setLength(length);
    }

    // We are currently using a map - check whether we still want to be doing so.
    // We will continue  to use a sparse map if SparseMode is set, a vector would be too sparse, or if allocation fails.
    unsigned numValuesInArray = storage->m_numValuesInVector + map->size();
    if (map->sparseMode() || attributes || !isDenseEnoughForVector(length, numValuesInArray) || !increaseVectorLength(vm, length))
        RELEASE_AND_RETURN(scope, map->putDirect(globalObject, this, i, value, attributes, mode));

    // Reread m_storage after increaseVectorLength, update m_numValuesInVector.
    storage = arrayStorage();
    storage->m_numValuesInVector = numValuesInArray;

    // Copy all values from the map into the vector, and delete the map.
    WriteBarrier<Unknown>* vector = storage->m_vector;
    SparseArrayValueMap::const_iterator end = map->end();
    for (SparseArrayValueMap::const_iterator it = map->begin(); it != end; ++it)
        vector[it->index()].set(vm, this, it->getNonSparseMode());
    deallocateSparseIndexMap();

    // Store the new property into the vector.
    WriteBarrier<Unknown>& valueSlot = vector[i];
    if (!valueSlot)
        ++storage->m_numValuesInVector;
    valueSlot.set(vm, this, value);
    if (returnsFromArrayStorageWhenDense(vm.arrayStoragePolicy()))
        tryConvertArrayStorageToContiguous(vm);
    return true;
}

bool JSObject::putDirectIndexSlowOrBeyondVectorLength(JSGlobalObject* globalObject, unsigned i, JSValue value, unsigned attributes, PutDirectIndexMode mode)
{
    VM& vm = globalObject->vm();
    ASSERT(!value.isCustomGetterSetter());

    // (An object with immutable properties: [[DefineOwnProperty]] succeeds for it if it changes nothing.)
    if (!canDoFastPutDirectIndex(this) || structure()->hasImmutableIndexedProperties()) {
        PropertyDescriptor descriptor;
        descriptor.setDescriptor(value, attributes);
        return methodTable()->defineOwnProperty(this, globalObject, Identifier::from(vm, i), descriptor, mode == PutDirectIndexShouldThrow);
    }

    // i should be a valid array index that is outside of the current vector.
    ASSERT(i <= MAX_ARRAY_INDEX);
    
    if (attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor))
        notifyPresenceOfIndexedAccessors(vm);
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES: {
        if (indexingShouldBeSparse() || attributes) {
            return putDirectIndexBeyondVectorLengthWithArrayStorage(
                globalObject, i, value, attributes, mode,
                ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        }
        if (!storesBeyondVectorInDenseStorage(vm.arrayStoragePolicy(), i, 0, 0)) {
            return putDirectIndexBeyondVectorLengthWithArrayStorage(
                globalObject, i, value, attributes, mode, createArrayStorage(vm, 0, 0));
        }
        if (needsSlowPutIndexing()) [[unlikely]] {
            ArrayStorage* storage = createArrayStorage(vm, i + 1, getNewVectorLength(0, 0, 0, i + 1));
            storage->m_vector[i].set(vm, this, value);
            storage->m_numValuesInVector++;
            return true;
        }
        
        createInitialForValueAndSet(vm, i, value);
        return true;
    }
        
    case ALL_UNDECIDED_INDEXING_TYPES: {
        convertUndecidedForValue(vm, value);
        // Reloop.
        return putDirectIndex(globalObject, i, value, attributes, mode);
    }
        
    case ALL_INT32_INDEXING_TYPES: {
        ASSERT(!indexingShouldBeSparse());
        if (attributes)
            return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        if (!value.isInt32()) {
            convertInt32ForValue(vm, value);
            return putDirectIndexSlowOrBeyondVectorLength(globalObject, i, value, attributes, mode);
        }
        putByIndexBeyondVectorLengthWithoutAttributes<Int32Shape>(globalObject, i, value);
        return true;
    }
        
    case ALL_DOUBLE_INDEXING_TYPES: {
        ASSERT(Options::allowDoubleShape());
        ASSERT(!indexingShouldBeSparse());
        if (attributes)
            return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        if (!value.isNumber()) {
            convertDoubleToContiguous(vm);
            return putDirectIndexSlowOrBeyondVectorLength(globalObject, i, value, attributes, mode);
        }
        double valueAsDouble = value.asNumber();
        if (valueAsDouble != valueAsDouble) {
            convertDoubleToContiguous(vm);
            return putDirectIndexSlowOrBeyondVectorLength(globalObject, i, value, attributes, mode);
        }
        putByIndexBeyondVectorLengthWithoutAttributes<DoubleShape>(globalObject, i, value);
        return true;
    }
        
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        ASSERT(!indexingShouldBeSparse());
        if (attributes)
            return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        putByIndexBeyondVectorLengthWithoutAttributes<ContiguousShape>(globalObject, i, value);
        return true;
    }

    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        if (attributes)
            return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, arrayStorage());
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
}

bool JSObject::putDirectNativeIntrinsicGetter(VM& vm, JSGlobalObject* globalObject, Identifier name, NativeFunction nativeFunction, Intrinsic intrinsic, unsigned attributes)
{
    JSFunction* function = JSFunction::create(vm, globalObject, 0, makeString("get "_s, name.string()), nativeFunction, ImplementationVisibility::Public, intrinsic);
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, function, nullptr);
    return putDirectNonIndexAccessor(vm, name, accessor, attributes);
}

void JSObject::putDirectNativeIntrinsicGetterWithoutTransition(VM& vm, JSGlobalObject* globalObject, Identifier name, NativeFunction nativeFunction, Intrinsic intrinsic, unsigned attributes)
{
    JSFunction* function = JSFunction::create(vm, globalObject, 0, makeString("get "_s, name.string()), nativeFunction, ImplementationVisibility::Public, intrinsic);
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, function, nullptr);
    putDirectNonIndexAccessorWithoutTransition(vm, name, accessor, attributes);
}

bool JSObject::putDirectNativeFunction(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, unsigned functionLength, NativeFunction nativeFunction, ImplementationVisibility implementationVisibility, Intrinsic intrinsic, unsigned attributes)
{
    StringImpl* name = propertyName.publicName();
    if (!name)
        name = vm.propertyNames->anonymous.impl();
    ASSERT(name);

    JSFunction* function = JSFunction::create(vm, globalObject, functionLength, name, nativeFunction, implementationVisibility, intrinsic);
    return putDirect(vm, propertyName, function, attributes);
}

bool JSObject::putDirectNativeFunction(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, unsigned functionLength, NativeFunction nativeFunction, ImplementationVisibility implementationVisibility, Intrinsic intrinsic, const DOMJIT::Signature* signature, unsigned attributes)
{
    StringImpl* name = propertyName.publicName();
    if (!name)
        name = vm.propertyNames->anonymous.impl();
    ASSERT(name);

    JSFunction* function = JSFunction::create(vm, globalObject, functionLength, name, nativeFunction, implementationVisibility, intrinsic, callHostFunctionAsConstructor, signature);
    return putDirect(vm, propertyName, function, attributes);
}

void JSObject::putDirectNativeFunctionWithoutTransition(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, unsigned functionLength, NativeFunction nativeFunction, ImplementationVisibility implementationVisibility, Intrinsic intrinsic, unsigned attributes)
{
    StringImpl* name = propertyName.publicName();
    if (!name)
        name = vm.propertyNames->anonymous.impl();
    ASSERT(name);
    JSFunction* function = JSFunction::create(vm, globalObject, functionLength, name, nativeFunction, implementationVisibility, intrinsic);
    putDirectWithoutTransition(vm, propertyName, function, attributes);
}

JSFunction* JSObject::putDirectBuiltinFunction(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, BuiltinCode code, unsigned attributes)
{
    JSFunction* function = JSFunction::create(vm, globalObject, code, globalObject);
    putDirect(vm, propertyName, function, attributes);
    return function;
}

JSFunction* JSObject::putDirectBuiltinFunctionWithoutTransition(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, BuiltinCode code, unsigned attributes)
{
    JSFunction* function = JSFunction::create(vm, globalObject, code, globalObject);
    putDirectWithoutTransition(vm, propertyName, function, attributes);
    return function;
}

// NOTE: This method is for ArrayStorage vectors.
ALWAYS_INLINE unsigned JSObject::getNewVectorLength(unsigned indexBias, unsigned currentVectorLength, unsigned currentLength, unsigned desiredLength)
{
    ASSERT(desiredLength <= MAX_STORAGE_VECTOR_LENGTH);

    unsigned increasedLength;
    unsigned maxInitLength = std::min(currentLength, 100000U);

    if (desiredLength < maxInitLength)
        increasedLength = maxInitLength;
    else if (!currentVectorLength)
        increasedLength = std::max(desiredLength, lastArraySize);
    else {
        increasedLength = timesThreePlusOneDividedByTwo(desiredLength);
    }

    ASSERT(increasedLength >= desiredLength);

    lastArraySize = std::min(increasedLength, FIRST_ARRAY_STORAGE_VECTOR_GROW);

    return ArrayStorage::optimalVectorLength(
        indexBias, structure()->outOfLineCapacity(),
        std::min(increasedLength, MAX_STORAGE_VECTOR_LENGTH));
}

ALWAYS_INLINE unsigned JSObject::getNewVectorLength(unsigned desiredLength)
{
    unsigned indexBias = 0;
    unsigned vectorLength = 0;
    unsigned length = 0;
    
    if (hasIndexedProperties(indexingType())) {
        if (ArrayStorage* storage = arrayStorageOrNull())
            indexBias = storage->m_indexBias;
        vectorLength = this->butterfly()->vectorLength();
        length = this->butterfly()->publicLength();
    }

    return getNewVectorLength(indexBias, vectorLength, length, desiredLength);
}

template<IndexingType indexingShape>
unsigned JSObject::countElements(Butterfly* butterfly)
{
    unsigned numValues = 0;
    for (unsigned i = butterfly->publicLength(); i--;) {
        switch (indexingShape) {
        case Int32Shape:
        case ContiguousShape:
            if (butterfly->contiguous().at(this, i))
                numValues++;
            break;
            
        case DoubleShape: {
            ASSERT(Options::allowDoubleShape());
            double value = butterfly->contiguousDouble().at(this, i);
            if (value == value)
                numValues++;
            break;
        }
            
        default:
            CRASH();
        }
    }
    return numValues;
}

unsigned JSObject::countElements()
{
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        return 0;
        
    case ALL_INT32_INDEXING_TYPES:
        return countElements<Int32Shape>(this->butterfly());
        
    case ALL_DOUBLE_INDEXING_TYPES:
        ASSERT(Options::allowDoubleShape());
        return countElements<DoubleShape>(this->butterfly());
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        return countElements<ContiguousShape>(this->butterfly());
        
    default:
        CRASH();
        return 0;
    }
}

bool JSObject::increaseVectorLength(VM& vm, unsigned newLength)
{
    ArrayStorage* storage = arrayStorage();
    
    unsigned vectorLength = storage->vectorLength();
    unsigned availableVectorLength = storage->availableVectorLength(structure(), vectorLength); 
    if (availableVectorLength >= newLength) {
        // The cell was already big enough for the desired length!
        for (unsigned i = vectorLength; i < availableVectorLength; ++i)
            storage->m_vector[i].clear();
        WTF::storeStoreFence();
        storage->setVectorLength(availableVectorLength);
        return true;
    }
    
    // This function leaves the array in an internally inconsistent state, because it does not move any values from sparse value map
    // to the vector. Callers have to account for that, because they can do it more efficiently.
    if (newLength > MAX_STORAGE_VECTOR_LENGTH)
        return false;

    if (newLength >= MIN_SPARSE_ARRAY_INDEX
        && !isDenseEnoughForVector(newLength, storage->m_numValuesInVector))
        return false;

    unsigned indexBias = storage->m_indexBias;
    ASSERT(newLength > vectorLength);
    unsigned newVectorLength = getNewVectorLength(newLength);

    // Fast case - there is no precapacity. In these cases a realloc makes sense.
    Structure* structure = this->structure();
    if (!indexBias) [[likely]] {
        DeferGC deferGC(vm);
        Butterfly* newButterfly = storage->butterfly()->growArrayRight(
            vm, this, structure, structure->outOfLineCapacity(), true,
            ArrayStorage::sizeFor(vectorLength), ArrayStorage::sizeFor(newVectorLength));
        if (!newButterfly)
            return false;
        for (unsigned i = vectorLength; i < newVectorLength; ++i)
            newButterfly->arrayStorage()->m_vector[i].clear();
        newButterfly->arrayStorage()->setVectorLength(newVectorLength);
        setButterfly(vm, newButterfly);
        return true;
    }
    
    // Remove some, but not all of the precapacity. Atomic decay, & capped to not overflow array length.
    DeferGC deferGC(vm);
    unsigned newIndexBias = std::min(indexBias >> 1, MAX_STORAGE_VECTOR_LENGTH - newVectorLength);
    Butterfly* newButterfly = storage->butterfly()->resizeArray(
        vm, this,
        structure->outOfLineCapacity(), true, ArrayStorage::sizeFor(vectorLength),
        newIndexBias, true, ArrayStorage::sizeFor(newVectorLength));
    if (!newButterfly)
        return false;
    for (unsigned i = vectorLength; i < newVectorLength; ++i)
        newButterfly->arrayStorage()->m_vector[i].clear();
    newButterfly->arrayStorage()->setVectorLength(newVectorLength);
    newButterfly->arrayStorage()->m_indexBias = newIndexBias;
    setButterfly(vm, newButterfly);
    return true;
}

bool JSObject::ensureLengthSlow(VM& vm, unsigned length)
{
    if (isCopyOnWrite(indexingMode())) {
        convertFromCopyOnWrite(vm);
        if (this->butterfly()->vectorLength() >= length)
            return true;
    }

    if (structure()->hasElementsStartOffset()) [[unlikely]] {
        unsigned vectorLength = Butterfly::optimalContiguousVectorLength(structure()->outOfLineCapacity() + 1, std::min<size_t>(nextLength(length), MAX_STORAGE_VECTOR_LENGTH));
        return tryReallocateWithElementsStartOffset(vm, vectorLength, 1);
    }

    Butterfly* butterfly = this->butterfly();
    
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    ASSERT(hasContiguous(indexingType()) || hasInt32(indexingType()) || hasDouble(indexingType()) || hasUndecided(indexingType()));
    ASSERT(length > butterfly->vectorLength());

    unsigned oldVectorLength = butterfly->vectorLength();
    unsigned newVectorLength;
    
    Structure* structure = this->structure();
    unsigned propertyCapacity = structure->outOfLineCapacity();
    
    GCDeferralContext deferralContext(vm);
    AssertNoGC assertNoGC;
    unsigned availableOldLength = isStorageInOwnCell(this, butterfly->base(0, propertyCapacity))
        ? oldVectorLength
        : Butterfly::availableContiguousVectorLength(propertyCapacity, oldVectorLength);
    Butterfly* newButterfly = nullptr;
    if (availableOldLength >= length) {
        // This is the case where someone else selected a vector length that caused internal
        // fragmentation. If we did our jobs right, this would never happen. But I bet we will mess
        // this up, so this defense should stay.
        newVectorLength = availableOldLength;
    } else {
        newVectorLength = Butterfly::optimalContiguousVectorLength(
            propertyCapacity, std::min<size_t>(nextLength(length), MAX_STORAGE_VECTOR_LENGTH));
        butterfly = butterfly->reallocArrayRightIfPossible(
            vm, deferralContext, this, structure, propertyCapacity, true,
            oldVectorLength * sizeof(EncodedJSValue),
            newVectorLength * sizeof(EncodedJSValue));
        if (!butterfly)
            return false;
        newButterfly = butterfly;
    }

    if (hasDouble(indexingType())) {
        for (unsigned i = oldVectorLength; i < newVectorLength; ++i)
            butterfly->indexingPayload<double>()[i] = PNaN;
    } else {
        for (unsigned i = oldVectorLength; i < newVectorLength; ++i)
            butterfly->indexingPayload<WriteBarrier<Unknown>>()[i].clear();
    }

    if (newButterfly) {
        butterfly->setVectorLength(newVectorLength);
        WTF::storeStoreFence();
        butterflyRef().set(vm, this, newButterfly);
    } else {
        WTF::storeStoreFence();
        butterfly->setVectorLength(newVectorLength);
    }

    return true;
}

void JSObject::reallocateAndShrinkButterfly(VM& vm, unsigned length)
{
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    ASSERT(hasContiguous(indexingType()) || hasInt32(indexingType()) || hasDouble(indexingType()) || hasUndecided(indexingType()));
    ASSERT(this->butterfly()->vectorLength() > length);
    ASSERT(this->butterfly()->publicLength() >= length);

    if (structure()->hasElementsStartOffset()) [[unlikely]] {
        bool didReallocate = tryReallocateWithElementsStartOffset(vm, length, 1);
        RELEASE_ASSERT(didReallocate);
        return;
    }
    ASSERT(!this->butterfly()->preCapacity(structure()));

    DeferGC deferGC(vm);
    Butterfly* newButterfly = this->butterfly()->resizeArray(vm, this, structure(), 0, ArrayStorage::sizeFor(length));
    newButterfly->setVectorLength(length);
    newButterfly->setPublicLength(length);
    WTF::storeStoreFence();
    butterflyRef().set(vm, this, newButterfly);

}

Butterfly* JSObject::allocateMoreOutOfLineStorage(VM& vm, size_t oldSize, size_t newSize)
{
    ASSERT(newSize > oldSize);

    // It's important that this function not rely on structure(), for the property
    // capacity, since we might have already mutated the structure in-place.

    return Butterfly::createOrGrowPropertyStorage(this->butterfly(), vm, this, structure(), oldSize, newSize);
}

bool JSObject::getOwnPropertyDescriptor(JSGlobalObject* globalObject, PropertyName propertyName, PropertyDescriptor& descriptor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);

    bool result = methodTable()->getOwnPropertySlot(this, globalObject, propertyName, slot);
    EXCEPTION_ASSERT_UNUSED(scope, !scope.exception() || !result);
    if (!result)
        return false;

    RELEASE_AND_RETURN(scope, descriptor.setPropertySlot(globalObject, propertyName, slot));
}

bool JSObject::putDirectMayBeIndex(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value)
{
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return putDirectIndex(globalObject, index.value(), value);
    return putDirect(globalObject->vm(), propertyName, value);
}

// True if every field of the descriptor is already in the current one with the same value.
static bool isPropertyUnchangedByDescriptor(JSGlobalObject* globalObject, const PropertyDescriptor& current, const PropertyDescriptor& descriptor)
{
    if (descriptor.enumerablePresent() && descriptor.enumerable() != current.enumerable())
        return false;
    if (descriptor.configurablePresent() && descriptor.configurable() != current.configurable())
        return false;
    if (descriptor.isAccessorDescriptor()) {
        if (!current.isAccessorDescriptor())
            return false;
        if (descriptor.getterPresent() && descriptor.getter() != current.getter())
            return false;
        if (descriptor.setterPresent() && descriptor.setter() != current.setter())
            return false;
        return true;
    }
    if (descriptor.isDataDescriptor()) {
        if (!current.isDataDescriptor())
            return false;
        if (descriptor.writablePresent() && descriptor.writable() != current.writable())
            return false;
        if (descriptor.value() && !sameValue(globalObject, descriptor.value(), current.value()))
            return false;
    }
    return true;
}

// What Object.freeze and Object.seal define, and whoever writes the same out by hand: the value and the accessor functions that the property has, and no attribute that admits more than the property admits now.
static bool onlyRepeatsOrRestrictsProperty(JSGlobalObject* globalObject, const PropertyDescriptor& current, const PropertyDescriptor& descriptor)
{
    if (descriptor.enumerablePresent() && descriptor.enumerable() != current.enumerable())
        return false;
    if (descriptor.configurablePresent() && descriptor.configurable() && !current.configurable())
        return false;
    if (descriptor.isAccessorDescriptor()) {
        if (!current.isAccessorDescriptor())
            return false;
        return (!descriptor.getterPresent() || descriptor.getter() == current.getter()) && (!descriptor.setterPresent() || descriptor.setter() == current.setter());
    }
    if (!descriptor.isDataDescriptor())
        return true;
    if (!current.isDataDescriptor() || (descriptor.writablePresent() && descriptor.writable() && !current.writable()))
        return false;
    return !descriptor.value() || sameValue(globalObject, descriptor.value(), current.value());
}

// https://tc39.es/ecma262/#sec-validateandapplypropertydescriptor
bool validateAndApplyPropertyDescriptor(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, bool isExtensible,
    const PropertyDescriptor& descriptor, bool isCurrentDefined, const PropertyDescriptor& current, bool throwException)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // If we have a new property we can just put it on normally
    // Step 2.
    if (!isCurrentDefined) {
        // unless extensions are prevented!
        // Step 2.a
        if (!isExtensible) {
            if (object && object->structure()->hasImmutableProperties()) [[unlikely]]
                return refuseChangeOfImmutableProperty(globalObject, scope, throwException, RefusalIs::Answered, object, propertyName, NonExtensibleObjectPropertyDefineError);
            return typeError(globalObject, scope, throwException, NonExtensibleObjectPropertyDefineError);
        }
        if (object && object->structure()->isImmutableProperty(propertyName.uid())) [[unlikely]]
            return refuseChangeOfImmutableProperty(globalObject, scope, throwException, RefusalIs::Answered, object, propertyName, ImmutablePropertyDefineError);

        if (object) {
            if (descriptor.isAccessorDescriptor()) {
                unsigned attributes = (descriptor.attributes() | PropertyAttribute::Accessor) & ~PropertyAttribute::ReadOnly;
                object->putDirectAccessor(globalObject, propertyName, descriptor.slowGetterSetter(globalObject), attributes);
            } else {
                ASSERT(descriptor.isGenericDescriptor() || descriptor.isDataDescriptor());
                JSValue value = descriptor.value() ? descriptor.value() : jsUndefined();
                object->putDirect(vm, propertyName, value, descriptor.attributes() & ~PropertyAttribute::Accessor);
            }
        }

        return true;
    }
    // Step 3.
    if (descriptor.isEmpty())
        return true;

    bool isEqual = current.equalTo(globalObject, descriptor);
    RETURN_IF_EXCEPTION(scope, false);
    if (isEqual)
        return true;

    // An object with immutable properties accepts a definition that changes nothing and one that only restricts the property further, as Object.freeze() makes. It refuses every other one.
    bool restrictsImmutableProperty = false;
    if (object && object->structure()->isImmutableProperty(propertyName.uid())) [[unlikely]] {
        bool isUnchanged = isPropertyUnchangedByDescriptor(globalObject, current, descriptor);
        RETURN_IF_EXCEPTION(scope, false);
        if (isUnchanged)
            return true;
        bool onlyRestricts = onlyRepeatsOrRestrictsProperty(globalObject, current, descriptor);
        RETURN_IF_EXCEPTION(scope, false);
        if (!onlyRestricts)
            return refuseChangeOfImmutableProperty(globalObject, scope, throwException, RefusalIs::Answered, object, propertyName, ImmutablePropertyDefineError);
        restrictsImmutableProperty = true;
    }

    // Step 4.
    if (!current.configurable()) {
        if (descriptor.configurable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeConfigurabilityError);
        if (descriptor.enumerablePresent() && descriptor.enumerable() != current.enumerable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeEnumerabilityError);
    }

    if (descriptor.isGenericDescriptor()) {
        // Step 5.
        // Changing [[Enumerable]] and [[Configurable]] attributes of an existing property
    } else if (current.isDataDescriptor() != descriptor.isDataDescriptor()) {
        // Step 6.
        // Changing between a data property and accessor property
        if (!current.configurable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeAccessMechanismError);
    } else if (current.isDataDescriptor() && descriptor.isDataDescriptor()) {
        // Step 7.
        // Changing the value and attributes of an existing data property
        if (!current.configurable() && !current.writable()) {
            if (descriptor.writable())
                return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeWritabilityError);
            if (descriptor.value()) {
                bool isSame = sameValue(globalObject, descriptor.value(), current.value());
                RETURN_IF_EXCEPTION(scope, false);
                if (!isSame)
                    return typeError(globalObject, scope, throwException, ReadonlyPropertyChangeError);
            }

            return true;
        }
    } else {
        // Step 8.
        // Changing the accessor functions and attributes of an existing accessor property
        ASSERT(descriptor.isAccessorDescriptor());
        if (!current.configurable()) {
            if (descriptor.setterPresent() && descriptor.setter() != current.setter())
                return typeError(globalObject, scope, throwException, "Attempting to change the setter of an unconfigurable property."_s);
            if (descriptor.getterPresent() && descriptor.getter() != current.getter())
                return typeError(globalObject, scope, throwException, "Attempting to change the getter of an unconfigurable property."_s);

            return true;
        }
    }

    if (!object)
        return true;
    // Step 9.
    std::optional<AllowLazyMaterializationOfImmutableProperties> admitsNewAttributes;
    if (restrictsImmutableProperty) [[unlikely]] {
        admitsNewAttributes.emplace(vm);
        // The attributes of an element lie with the element.
        if (parseIndex(propertyName))
            RELEASE_AND_RETURN(scope, JSObject::defineOwnProperty(object, globalObject, propertyName, descriptor, throwException));
        // An array answers for its length by itself. (JSArray::defineOwnProperty(): one whose every property is immutable stays as it is.)
        if (propertyName == vm.propertyNames->length && isJSArray(object))
            return true;
    }
    unsigned attributes = descriptor.attributesOverridingCurrent(current);
    if (descriptor.isAccessorDescriptor() || (current.isAccessorDescriptor() && !descriptor.isDataDescriptor())) {
        ASSERT(attributes & PropertyAttribute::Accessor);
        JSObject* getter = descriptor.getterPresent() ? descriptor.getterObject() : (current.getterPresent() ? current.getterObject() : nullptr);
        JSObject* setter = descriptor.setterPresent() ? descriptor.setterObject() : (current.setterPresent() ? current.setterObject() : nullptr);
        GetterSetter* getterSetter = GetterSetter::create(vm, globalObject, getter, setter);
        object->putDirectAccessor(globalObject, propertyName, getterSetter, attributes & ~PropertyAttribute::ReadOnly);
    } else {
        ASSERT(descriptor.isGenericDescriptor() || descriptor.isDataDescriptor());
        JSValue value = descriptor.value() ? descriptor.value() : (current.value() ? current.value() : jsUndefined());
        object->putDirect(vm, propertyName, value, attributes & ~PropertyAttribute::Accessor);
    }

    return true;
}

bool JSObject::defineOwnNonIndexProperty(JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool throwException)
{
    VM& vm  = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    PropertyDescriptor current;
    bool isCurrentDefined = getOwnPropertyDescriptor(globalObject, propertyName, current);
    RETURN_IF_EXCEPTION(throwScope, false);
    bool isExtensible = this->isExtensible(globalObject);
    RETURN_IF_EXCEPTION(throwScope, false);
    RELEASE_AND_RETURN(throwScope, validateAndApplyPropertyDescriptor(globalObject, this, propertyName, isExtensible, descriptor, isCurrentDefined, current, throwException));
}

bool JSObject::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool throwException)
{
    ASSERT(!object->structure()->isCompiledStructure());
    // If it's an array index, then use the indexed property storage.
    if (std::optional<uint32_t> index = parseIndex(propertyName)) {
        // c. Let succeeded be the result of calling the default [[DefineOwnProperty]] internal method (8.12.9) on A passing P, Desc, and false as arguments.
        // d. Reject if succeeded is false.
        // e. If index >= oldLen
        // e.i. Set oldLenDesc.[[Value]] to index + 1.
        // e.ii. Call the default [[DefineOwnProperty]] internal method (8.12.9) on A passing "length", oldLenDesc, and false as arguments. This call will always return true.
        // f. Return true.
        return object->defineOwnIndexedProperty(globalObject, index.value(), descriptor, throwException);
    }
    
    return object->defineOwnNonIndexProperty(globalObject, propertyName, descriptor, throwException);
}

void JSObject::convertToDictionary(VM& vm)
{
    Structure* oldStructure = structure();
    DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, oldStructure);
    setStructure(vm, Structure::toCacheableDictionaryTransition(vm, oldStructure, &deferredWatchpointFire));
}

void JSObject::convertToUncacheableDictionary(VM& vm)
{
    Structure* oldStructure = structure();
    if (oldStructure->isUncacheableDictionary())
        return;
    DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, oldStructure);
    setStructure(vm, Structure::toUncacheableDictionaryTransition(vm, oldStructure, &deferredWatchpointFire));
    if (mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change, this);
}


void JSObject::shiftButterflyAfterFlattening(const ConcurrentJSLocker&, VM& vm, Structure* structure, size_t outOfLineCapacityAfter)
{
    // This could interleave visitChildren because some old structure could have been a non
    // dictionary structure. We have to be crazy careful. But, we are guaranteed to be holding
    // the structure's lock right now, and that helps a bit.

    Butterfly* oldButterfly = this->butterfly();
    size_t preCapacity;
    size_t indexingPayloadSizeInBytes;
    bool hasIndexingHeader = this->hasIndexingHeader();
    if (hasIndexingHeader) [[unlikely]] {
        preCapacity = structure->hasElementsStartOffset() ? 1 : oldButterfly->preCapacity(structure);
        indexingPayloadSizeInBytes = oldButterfly->indexingPayloadSizeInBytes(structure);
    } else {
        preCapacity = 0;
        indexingPayloadSizeInBytes = 0;
    }

    Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, preCapacity, outOfLineCapacityAfter, hasIndexingHeader, indexingPayloadSizeInBytes);
    if (structure->hasElementsStartOffset()) [[unlikely]]
        newButterfly->indexingHeader()->elementsStartOffset(outOfLineCapacityAfter) = preCapacity;

    // No need to copy the precapacity.
    void* currentBase = oldButterfly->base(0, outOfLineCapacityAfter);
    void* newBase = newButterfly->base(0, outOfLineCapacityAfter);

    // memcpy is fine since newButterfly is not tied to any object yet.
    memcpy(static_cast<JSValue*>(newBase), static_cast<JSValue*>(currentBase), Butterfly::totalSize(0, outOfLineCapacityAfter, hasIndexingHeader, indexingPayloadSizeInBytes));
    
    setButterfly(vm, newButterfly);
}

uint32_t JSObject::getEnumerableLength()
{
    JSObject* object = this;

    switch (object->indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        // Regardless of holesMustForwardToPrototype condition, it returns zero.
        return 0;
        
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = object->butterfly();
        unsigned enumerableLength = butterfly->publicLength();
        if (!enumerableLength)
            return 0;
        if (object->structure()->holesMustForwardToPrototype(object))
            return 0;
        for (unsigned i = 0; i < enumerableLength; ++i) {
            if (!butterfly->contiguous().at(object, i))
                return 0;
        }
        return enumerableLength;
    }
        
    case ALL_DOUBLE_INDEXING_TYPES: {
        Butterfly* butterfly = object->butterfly();
        unsigned enumerableLength = butterfly->publicLength();
        if (!enumerableLength)
            return 0;
        if (object->structure()->holesMustForwardToPrototype(object))
            return 0;
        for (unsigned i = 0; i < enumerableLength; ++i) {
            double value = butterfly->contiguousDouble().at(object, i);
            if (value != value)
                return 0;
        }
        return enumerableLength;
    }
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = object->butterfly()->arrayStorage();
        if (storage->m_sparseMap.get())
            return 0;
        
        unsigned enumerableLength = std::min(storage->length(), storage->vectorLength());
        if (!enumerableLength)
            return 0;
        if (object->structure()->holesMustForwardToPrototype(object))
            return 0;
        for (unsigned i = 0; i < enumerableLength; ++i) {
            if (!storage->m_vector[i])
                return 0;
        }
        return enumerableLength;
    }
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }
}

// Implements GetMethod(O, P) in section 7.3.9 of the spec.
// http://www.ecma-international.org/ecma-262/6.0/index.html#sec-getmethod
JSValue JSObject::getMethod(JSGlobalObject* globalObject, CallData& callData, const Identifier& ident, const String& errorMessage)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue method = get(globalObject, ident);
    RETURN_IF_EXCEPTION(scope, JSValue());

    if (!method.isCell()) {
        if (method.isUndefinedOrNull())
            return jsUndefined();

        throwVMTypeError(globalObject, scope, errorMessage);
        return jsUndefined();
    }

    callData = JSC::getCallDataInline(method.asCell());
    if (callData.type == CallData::Type::None) {
        throwVMTypeError(globalObject, scope, errorMessage);
        return jsUndefined();
    }

    return method;
}

bool JSObject::anyObjectInChainMayInterceptIndexedAccesses() const
{
    for (const JSObject* current = this; ;) {
        if (current->structure()->mayInterceptIndexedAccesses() || current->hasFrozenElementsInPlace() || JSGlobalObject::isTakenToInterceptIndexedAccesses(current))
            return true;
        if (isTypedArrayType(current->type())) [[unlikely]] {
            auto* globalObject = realmMayBeNull();
            if (globalObject && globalObject->typedArraysInterceptIndexedStores())
                return true;
        }
#if ENABLE(AOT)
        if (AOT::PropertyWalk::isCompiledObject(current)) [[unlikely]] {
            JSObject* holder = AOT::PropertyWalk::objectHoldingIndexedProperties(current);
            if (holder && holder->structure()->mayInterceptIndexedAccesses())
                return true;
        }
#endif

        JSValue prototype = current->getPrototypeDirect();
        if (prototype.isNull())
            return false;

        current = asObject(prototype);
    }
}

bool JSObject::needsSlowPutIndexing() const
{
    if (anyObjectInChainMayInterceptIndexedAccesses())
        return true;
    auto* globalObject = realmMayBeNull();
    return globalObject && globalObject->isHavingABadTime();
}

TransitionKind JSObject::suggestedArrayStorageTransition() const
{
    if (needsSlowPutIndexing())
        return TransitionKind::AllocateSlowPutArrayStorage;
    
    return TransitionKind::AllocateArrayStorage;
}

void JSObject::putOwnDataPropertyBatching(VM& vm, UniquedStringImpl** properties, const EncodedJSValue* values, unsigned size)
{
    unsigned i = 0;
    Structure* structure = this->structure();
    bool canBatch = !(structure->isCompiledStructure() || structure->isDictionary() || (structure->transitionCountEstimate() + size) > Structure::s_maxTransitionLength || !structure->canPerformFastPropertyEnumerationCommon());
    if (canBatch) {
        Vector<PropertyOffset, 16> offsets(size, [&](size_t index) -> std::optional<PropertyOffset> {
            PropertyName propertyName(properties[index]);

            PropertyOffset offset;
            if (Structure* newStructure = Structure::addPropertyTransitionToExistingStructure(structure, propertyName, 0, offset)) {
                structure = newStructure;
                return offset;
            }

            unsigned currentAttributes;
            offset = structure->get(vm, propertyName, currentAttributes);
            if (offset != invalidOffset) {
                structure->didReplaceProperty(offset);
                return offset;
            }

            // If we detect that this structure requires transition watchpoint firing, then we need to stop this batching and rest of the values
            // should be put via generic way.
            if (structure->transitionWatchpointSet().isBeingWatched() && structure->transitionWatchpointSet().isStillValid()) [[unlikely]]
                return std::nullopt;

            // It will go to the cacheable dictionary case. We stop the batching here and fall though to the generic case.
            // We break here before adding offset to offsets since this property itself should be put via generic path.
            if (structure->shouldDoCacheableDictionaryTransitionForAdd(PutPropertySlot::UnknownContext)) [[unlikely]]
                return std::nullopt;

            Structure* newStructure = Structure::addNewPropertyTransition(vm, structure, propertyName, 0, offset, PutPropertySlot::UnknownContext, nullptr);

            validateOffset(offset);
            ASSERT(newStructure->isValidOffset(offset));

            structure = newStructure;
            return offset;
        }, NulloptBehavior::Abort);

        // Flush batching here. Note that it is possible that offsets.size() is not equal to size, if we stop batching due to transition-watchpoint-firing.

        Butterfly* newButterfly = this->butterfly();
        auto* oldStructure = this->structure();
        if (oldStructure->outOfLineCapacity() != structure->outOfLineCapacity()) {
            ASSERT(structure != oldStructure);
            newButterfly = allocateMoreOutOfLineStorage(vm, oldStructure->outOfLineCapacity(), structure->outOfLineCapacity());
            nukeStructureAndSetButterfly(vm, StructureID::encode(oldStructure), newButterfly);
        }

        for (unsigned index = 0; index < offsets.size(); ++index)
            putDirectOffset(vm, offsets[index], JSValue::decode(values[index]));
        setStructure(vm, structure);

        // We fall through to the generic case and consume the rest of put operations if batching stopped in the middle.
        i = offsets.size();

        if (mayBePrototype())
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add, this, std::span<UniquedStringImpl* const> { properties, i });
    }

    for (; i < size; ++i) {
        PutPropertySlot putPropertySlot(this, true);
        putOwnDataProperty(vm, properties[i], JSValue::decode(values[i]), putPropertySlot);
    }
}

void JSObject::setStructureForAddedPropertySlow(VM& vm, Structure* previous, Structure* next, PropertyOffset offset, PutPropertySlot& slot)
{
    setStructure(vm, next);
    if (next->isCacheableTransitionFrom(previous))
        slot.setNewProperty(this, offset);
}

ASCIILiteral JSObject::putDirectToDictionaryWithoutExtensibility(VM& vm, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    unsigned currentAttributes;
    Structure* structure = this->structure();
    PropertyOffset offset = structure->get(vm, propertyName, currentAttributes);
    if (offset != invalidOffset) {
        if (currentAttributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor)
            return ReadonlyPropertyChangeError;

        putDirectOffset(vm, offset, value);
        structure->didReplaceProperty(offset);

        // FIXME: Check attributes against PropertyAttribute::CustomAccessorOrValue. Changing GetterSetter should work w/o transition.
        // https://bugs.webkit.org/show_bug.cgi?id=214342
        ASSERT(!(currentAttributes & PropertyAttribute::AccessorOrCustomAccessorOrValue));
        slot.setExistingProperty(this, offset);
        return { };
    }

    return NonExtensibleObjectPropertyDefineError;
}

#if CPU(ARM64) && CPU(ADDRESS64)
template <size_t N>
static uint64_t saturate(uint64_t value)
{
    static_assert(N < 64);
    if (value < (1ul << N))
        return value;
    return (1ul << N) - 1;
}

// Formal nibble layout of the crash-info registers filled in below and passed to WTFCrashWithInfo.
// Every field occupies whole nibbles and no two fields share a nibble, so each reads straight out of
// the hex register dump. The constants are nibble indices (the low nibble of each field);
// nibbleShift() turns an index into a bit shift. reg3 (raw cell header) and reg6 (this) aren't packed.
namespace EmptyValueCrashLayout {

constexpr unsigned nibbleShift(unsigned nibbleIndex) { return nibbleIndex * 4; }

// Per-object GC state returned by gcStateFor(), one nibble per boolean (nibbles 0-5).
namespace GCState {
constexpr unsigned isPreciseAllocation = 0;
constexpr unsigned isMarked = 1;
constexpr unsigned isNewlyAllocated = 2; // meaningful while marking
constexpr unsigned isFreeListed = 3; // MarkedBlock only: block still being allocated out of
constexpr unsigned isAllocated = 4; // MarkedBlock only: block was allocated from, now full
constexpr unsigned isOnFreeList = 5; // MarkedBlock + freeListed only: cell is a free (dead) cell
constexpr unsigned nibbleCount = 6;
}

// reg1: offending offset + bad-object data validity + butterfly state + structure dictionary state.
namespace Reg1 {
constexpr unsigned propertyOffset = 0; // nibbles 0-3 (16 bits)
constexpr unsigned offsetIsInline = 4; // 0 => out-of-line (in the butterfly)
constexpr unsigned structureIDMapsToValidStructure = 5; // this->structureID().tryDecode() != null
constexpr unsigned structureOfStructureIsStructureStructure = 6;
constexpr unsigned badObjectIsZeroFilled = 7; // every word of the cell is zero
constexpr unsigned blockHeaderVMPointerState = 8; // MarkedBlock only: 0 => zero, 1 => non-null but not equal to structure's VM, 2 => equal to structure's VM
constexpr unsigned butterflyIsNull = 9; // out-of-line only
constexpr unsigned butterflyInButterflySpace = 10; // out-of-line only: in vm.auxiliarySpace()
constexpr unsigned butterflyOutOfLineStorageIsZeroFilled = 11; // out-of-line only
constexpr unsigned structureIsDictionary = 12;
constexpr unsigned structureIsUncacheableDictionary = 13; // the kind flattenDictionaryStructure() renumbers
constexpr unsigned structureHasBeenFlattenedBefore = 14; // ineligible for further flattening
constexpr unsigned structureInlineCapacity = 15; // saturated to 4 bits
}

// reg2: sizes/counts, 16 bits (4 nibbles) each.
namespace Reg2 {
constexpr unsigned blankCellWordCount = 0; // nibbles 0-3: zero 8-byte words in the cell
constexpr unsigned outOfLineSize = 4; // nibbles 4-7: structure's out-of-line slot count
constexpr unsigned butterflyPublicLength = 8; // nibbles 8-11: out-of-line + indexed only
constexpr unsigned butterflyVectorLength = 12; // nibbles 12-15: out-of-line + indexed only
}

// reg4: GC state of the bad object and its prototype-chain child.
namespace Reg4 {
constexpr unsigned badObjectGCState = 0; // nibbles 0-5
constexpr unsigned previousInChainGCState = GCState::nibbleCount; // nibbles 6-11
constexpr unsigned blockHeaderZeroByteCount = 12; // nibbles 12-15 (16 bits): number of zero bytes in the block header, MarkedBlock only
}

// reg5: GC state of the base object + global GC state + chain metadata.
namespace Reg5 {
constexpr unsigned bottomOfChainGCState = 0; // nibbles 0-5
constexpr unsigned isMarking = 6; // MarkedSpace::isMarking()
constexpr unsigned previousInChainIsNull = 7;
constexpr unsigned badObjectIsBottomOfChain = 8;
constexpr unsigned chainDepth = 9; // nibbles 9-10 (8 bits); 0xff => not reached within cap
constexpr unsigned collectionScope = 11; // 0 => none, 1 => eden, 2 => full
constexpr unsigned worldIsStopped = 12;
constexpr unsigned mutatorState = 13; // 0 => running, 1 => allocating, 2 => sweeping, 3 => collecting
constexpr unsigned previousSlotClassification = 14; // see classifySlotWord
constexpr unsigned nextSlotClassification = 15; // ditto
}

// The layout must stay nibble-aligned with no field overlapping the next.
static_assert(Reg4::previousInChainGCState == Reg4::badObjectGCState + GCState::nibbleCount);
static_assert(Reg5::isMarking == Reg5::bottomOfChainGCState + GCState::nibbleCount);
static_assert(Reg5::chainDepth + 2 <= Reg5::collectionScope); // chainDepth spans 2 nibbles
static_assert(Reg5::nextSlotClassification <= 15);
static_assert(Reg5::nextSlotClassification == Reg5::previousSlotClassification + 1);
static_assert(Reg1::offsetIsInline == Reg1::propertyOffset + 4); // propertyOffset spans 4 nibbles
static_assert(Reg2::outOfLineSize == Reg2::blankCellWordCount + 4); // 16-bit fields span 4 nibbles
static_assert(Reg2::butterflyPublicLength == Reg2::outOfLineSize + 4);
static_assert(Reg2::butterflyVectorLength + 4 <= 16);
static_assert(Reg4::blockHeaderZeroByteCount == Reg4::previousInChainGCState + GCState::nibbleCount);
static_assert(Reg4::blockHeaderZeroByteCount + 4 <= 16); // 16-bit count spans 4 nibbles
static_assert(Reg1::structureIsDictionary == Reg1::butterflyOutOfLineStorageIsZeroFilled + 1);
static_assert(Reg1::structureIsUncacheableDictionary == Reg1::structureIsDictionary + 1);
static_assert(Reg1::structureHasBeenFlattenedBefore == Reg1::structureIsUncacheableDictionary + 1);
static_assert(Reg1::structureInlineCapacity == Reg1::structureHasBeenFlattenedBefore + 1);
static_assert(Reg1::structureInlineCapacity <= 15);

}
#endif

NO_RETURN_DUE_TO_CRASH NEVER_INLINE void JSObject::crashDueToEmptyValueAtValidOffset(Structure* structure, PropertyName propertyName, PropertyOffset offset, JSObject* bottomOfChain, JSObject* previousInChain, unsigned attributes, int line, const char* filename, const char* function_name)
{
#if CPU(ARM64) && CPU(ADDRESS64)
    register volatile uint64_t reg1 __asm__(CRASH_GPR1) { };
    register volatile uint64_t reg2 __asm__(CRASH_GPR2) { };
    register volatile uint64_t reg3 __asm__(CRASH_GPR3) { };
    register volatile uint64_t reg4 __asm__(CRASH_GPR4) { };
    register volatile uint64_t reg5 __asm__(CRASH_GPR5) { };
    register volatile uint64_t reg6 __asm__(CRASH_GPR6) { };
    register volatile uint64_t dumpState __asm__("x28") { };
#define updateDumpState(newState) do { \
        WTF::compilerFence(); \
        __asm__ volatile("" :: "r"(reg1), "r"(reg2), "r"(reg3), "r"(reg4), "r"(reg5), "r"(reg6)); \
        __asm__ volatile( \
            "mov %0, #" #newState "\n\t" \
            "orr %0, %0, #0xffffffffffff0000" \
            : "=r"(dumpState)); \
        WTF::compilerFence(); \
    } while (false)

    updateDumpState(0x5700);

    reg6 = reinterpret_cast<uint64_t>(this);

    // Fields are appended from least likely to fault while computing to most likely, so if we crash
    // partway through, every append so far is preserved in the crash registers. reg6 = this (the bad
    // object); reg3 = its raw first 8 bytes (m_structureID & m_blob). The nibble layout of the packed
    // registers is captured formally in the EmptyValueCrashLayout namespace above; the shifts below
    // are derived from it via nibbleShift().
    using namespace EmptyValueCrashLayout;

    JSObject* badObject = this;
    VM* vm = std::bit_cast<uint64_t>(structure) > 0x10000 ? &structure->vm() : nullptr;

    reg3 = *std::bit_cast<uint64_t*>(badObject); // raw cell header: m_structureID + m_blob

    updateDumpState(0x5701);

    reg1 |= saturate<16>(offset) << nibbleShift(Reg1::propertyOffset);
    reg1 |= static_cast<uint64_t>(isInlineOffset(offset)) << nibbleShift(Reg1::offsetIsInline);

    updateDumpState(0x5702);

    // Global GC state: Are we marking? What is the collection scope? Is the world stopped? What is the mutator doing?
    // This could catch a concurrent-collector race that could clear a slot.
    bool heapIsMarking = vm != nullptr && vm->heap.objectSpace().isMarking();
    reg5 |= static_cast<uint64_t>(heapIsMarking) << nibbleShift(Reg5::isMarking);
    if (vm) {
        std::optional<CollectionScope> collectionScope = vm->heap.collectionScope();
        uint64_t collectionScopeCode = !collectionScope ? 0 : (*collectionScope == CollectionScope::Eden ? 1 : 2);
        reg5 |= collectionScopeCode << nibbleShift(Reg5::collectionScope);
        reg5 |= static_cast<uint64_t>(vm->heap.worldIsStopped()) << nibbleShift(Reg5::worldIsStopped);
        reg5 |= static_cast<uint64_t>(vm->heap.mutatorState()) << nibbleShift(Reg5::mutatorState);
    }

    // Allocation kind, mark/newly-allocated state, and (for MarkedBlock cells)
    // whether the block is still being allocated out of and whether the cell is
    // itself a free (dead) cell on the allocator's free list.
    auto gcStateFor = [](JSCell* cell) -> uint64_t {
        if (!cell)
            return 0;
        uint64_t state = 0;
        if (cell->isPreciseAllocation()) {
            auto& allocation = cell->preciseAllocation();
            state |= 1ull << nibbleShift(GCState::isPreciseAllocation); // isPreciseAllocation() == true
            state |= static_cast<uint64_t>(allocation.isMarked()) << nibbleShift(GCState::isMarked);
            state |= static_cast<uint64_t>(allocation.isNewlyAllocated()) << nibbleShift(GCState::isNewlyAllocated);
        } else {
            auto& block = cell->markedBlock();
            auto& handle = block.handle();
            state |= static_cast<uint64_t>(block.isMarked(cell)) << nibbleShift(GCState::isMarked);
            state |= static_cast<uint64_t>(block.isNewlyAllocated(cell)) << nibbleShift(GCState::isNewlyAllocated);
            bool freeListed = handle.isFreeListed();
            state |= static_cast<uint64_t>(freeListed) << nibbleShift(GCState::isFreeListed);
            state |= static_cast<uint64_t>(handle.isAllocated()) << nibbleShift(GCState::isAllocated);
            if (freeListed) {
                if (BlockDirectory* directory = handle.directory())
                    state |= static_cast<uint64_t>(directory->isFreeListedCell(cell)) << nibbleShift(GCState::isOnFreeList);
            }
        }
        return state;
    };

    updateDumpState(0x5703);

    uint64_t badObjectState = gcStateFor(badObject);
    reg4 |= badObjectState << nibbleShift(Reg4::badObjectGCState);

    updateDumpState(0x5704);

    uint64_t previousState = gcStateFor(previousInChain);
    reg4 |= previousState << nibbleShift(Reg4::previousInChainGCState);

    updateDumpState(0x5705);

    uint64_t bottomState = gcStateFor(bottomOfChain);
    reg5 |= bottomState << nibbleShift(Reg5::bottomOfChainGCState);

    reg5 |= static_cast<uint64_t>(!previousInChain) << nibbleShift(Reg5::previousInChainIsNull);
    reg5 |= static_cast<uint64_t>(bottomOfChain == badObject) << nibbleShift(Reg5::badObjectIsBottomOfChain);

    updateDumpState(0x5706);

    // Backtrack the prototype chain from the original baseValue to discover how far up the bad
    // object manifests. 0xff means we did not reach it within the cap (corrupt/cyclic chain).
    uint64_t chainDepth = 0;
    JSObject* chainCursor = bottomOfChain;
    {
        constexpr uint64_t maxDepth = 0xff;
        while (chainCursor && chainCursor != badObject && chainDepth < maxDepth) {
            Structure* cursorStructure = chainCursor->structureID().tryDecode();
            if (!cursorStructure)
                break;
            JSValue prototype = cursorStructure->storedPrototype(chainCursor);
            if (!prototype.isObject())
                break;
            chainCursor = asObject(prototype);
            ++chainDepth;
        }
        if (chainCursor != badObject)
            chainDepth = maxDepth;
    }
    reg5 |= saturate<8>(chainDepth) << nibbleShift(Reg5::chainDepth);

    updateDumpState(0x5707);

    // Validate the bad object's structure: does its StructureID decode to an allocated Structure,
    // and is that Structure itself described by the canonical structureStructure?
    StructureID badStructureID = badObject->structureID();
    Structure* decodedStructure = badStructureID.tryDecode();
    reg1 |= static_cast<uint64_t>(decodedStructure != nullptr) << nibbleShift(Reg1::structureIDMapsToValidStructure);

    updateDumpState(0x5708);

    Structure* metaStructure = nullptr;
    bool structureOfStructureIsStructureStructure = false;
    if (decodedStructure) {
        metaStructure = decodedStructure->structureID().tryDecode();
        structureOfStructureIsStructureStructure = metaStructure && vm != nullptr && metaStructure == vm->structureStructure.get();
        reg1 |= static_cast<uint64_t>(decodedStructure->isDictionary()) << nibbleShift(Reg1::structureIsDictionary);
        reg1 |= static_cast<uint64_t>(decodedStructure->isUncacheableDictionary()) << nibbleShift(Reg1::structureIsUncacheableDictionary);
        reg1 |= static_cast<uint64_t>(decodedStructure->hasBeenFlattenedBefore()) << nibbleShift(Reg1::structureHasBeenFlattenedBefore);
        reg1 |= saturate<4>(decodedStructure->inlineCapacity()) << nibbleShift(Reg1::structureInlineCapacity);
    }
    reg1 |= static_cast<uint64_t>(structureOfStructureIsStructureStructure) << nibbleShift(Reg1::structureOfStructureIsStructureStructure);

    updateDumpState(0x5709);

    // Is the bad object zero-filled? Count its zero 8-byte words.
    std::array<char, 8> emptyWord { };
    uint64_t blankCellWords = 0;
    size_t cellWords = badObject->cellSize() / 8;
    {
        auto* wordBase = reinterpret_cast<const char*>(badObject);
        for (size_t i = 0; i < cellWords; ++i) {
            if (equalSpans(unsafeMakeSpan(wordBase + i * 8, 8), std::span(emptyWord)))
                ++blankCellWords;
        }
    }
    reg1 |= static_cast<uint64_t>(cellWords && blankCellWords == cellWords) << nibbleShift(Reg1::badObjectIsZeroFilled);
    reg2 |= saturate<16>(blankCellWords) << nibbleShift(Reg2::blankCellWordCount);

    updateDumpState(0x570a);

    // If the bad object lives in a MarkedBlock, how zeroed-out is that block's header?
    // The header holds some words that are written non-zero even after the payload is
    // zero-filled, so zero bytes there are the stronger corruption signal. Also capture
    // the header's VM pointer, mirroring MarkedBlock::analyzeInvalidHandleAndCrash().
    if (!badObject->isPreciseAllocation()) {
        MarkedBlock& block = badObject->markedBlock();
        auto* blockStart = reinterpret_cast<const char*>(&block);

        auto countZeroBytes = [](const char* begin, const char* end) -> uint64_t {
            uint64_t zeros = 0;
            for (auto* p = begin; p < end; ++p) {
                if (!*p)
                    ++zeros;
            }
            return zeros;
        };
        uint64_t headerZeroBytes = countZeroBytes(blockStart + MarkedBlock::offsetOfHeader, blockStart + MarkedBlock::offsetOfHeader + MarkedBlock::headerSize);
        reg4 |= saturate<16>(headerZeroBytes) << nibbleShift(Reg4::blockHeaderZeroByteCount);

        // Is a critical value like the header's VM pointer zeroed out?
        // 0 => zeroed, 1 => non-null but not this structure's VM, 2 => matches `vm` from from the `structure` argument.
        VM* blockVM = *std::bit_cast<VM* const*>(blockStart + MarkedBlock::offsetOfHeader + MarkedBlock::Header::offsetOfVM());
        uint64_t blockVMState = !blockVM ? 0 : (blockVM == vm ? 2 : 1);
        reg1 |= blockVMState << nibbleShift(Reg1::blockHeaderVMPointerState);

        updateDumpState(0x570b);
    }

    // Classify a raw slot word read as a JSValue without dereferencing it:
    // 0 => empty,
    // 1 => non-cell value,
    // 2 => cell-tagged and pointing at a live heap cell (MarkedBlock or precise allocation),
    // 3 => cell-tagged but not a valid heap object,
    // 4 => cell-tagged precise-looking cell but the VM isn't available to find the precise-allocation set,
    // 5 => cell-tagged precise-looking cell but the precise-allocation set isn't available to verify.
    auto classifySlotWord = [&](uint64_t word) -> uint64_t {
        JSValue value = JSValue::decode(std::bit_cast<EncodedJSValue>(word));
        if (!value)
            return 0;
        if (!value.isCell())
            return 1;
        if (!vm)
            return 4;
        JSCell* candidate = value.asCell();
        if (candidate->isPreciseAllocation()) {
            auto& preciseSet = vm->heap.objectSpace().preciseAllocationSet();
            if (!preciseSet)
                return 5;
            return preciseSet->contains(candidate) ? 2 : 3;
        }
        if (!MarkedBlock::isAtomAligned(candidate))
            return 3;
        MarkedBlock* candidateBlock = MarkedBlock::blockFor(candidate);
        return vm->heap.objectSpace().blocks().set().contains(candidateBlock) ? 2 : 3;
    };

    // For an out-of-line offending offset, inspect the butterfly the null was fetched from.
    if (isOutOfLineOffset(offset)) {
        unsigned outOfLineSize = structure->outOfLineSize();
        reg2 |= saturate<16>(outOfLineSize) << nibbleShift(Reg2::outOfLineSize);

        Butterfly* butterfly = badObject->butterfly();
        reg1 |= static_cast<uint64_t>(!butterfly) << nibbleShift(Reg1::butterflyIsNull);

        updateDumpState(0x570c);

        // Is the butterfly in butterfly (auxiliary) space? Locate its MarkedBlock without
        // dereferencing the butterfly, and trust it only if that's a known live block.
        MarkedBlock* butterflyBlock = butterfly ? MarkedBlock::blockFor(butterfly) : nullptr;
        bool butterflyInButterflySpace = false;
        if (butterfly && vm) {
            const auto& blockSet = vm->heap.objectSpace().blocks().set();
            if (blockSet.contains(butterflyBlock))
                butterflyInButterflySpace = butterflyBlock->subspace() == &vm->auxiliarySpace();
        }
        reg1 |= static_cast<uint64_t>(butterflyInButterflySpace) << nibbleShift(Reg1::butterflyInButterflySpace);

        updateDumpState(0x570d);

        // Only read butterfly contents once we know it is a real butterfly.
        if (butterfly && butterflyInButterflySpace) {
            auto* storageBase = reinterpret_cast<const char*>(butterfly->propertyStorage());
            auto* blockStart = reinterpret_cast<const char*>(butterflyBlock);
            const char* blockEnd = blockStart + MarkedBlock::blockSize;

            // Out-of-line slots live at negative offsets from the property-storage base.
            // Is the whole out-of-line storage zero-filled? Stay within the block.
            if (outOfLineSize) {
                bool allZero = true;
                for (unsigned i = 1; i <= outOfLineSize; ++i) {
                    auto* slot = storageBase - i * 8;
                    if (slot < blockStart)
                        break;
                    if (!equalSpans(unsafeMakeSpan(slot, 8), std::span(emptyWord))) {
                        allZero = false;
                        break;
                    }
                }
                reg1 |= static_cast<uint64_t>(allZero) << nibbleShift(Reg1::butterflyOutOfLineStorageIsZeroFilled);
            }

            // Butterfly indexing header lengths.
            // Meaningful only when the object has an indexing header, which sits immediately before the property-storage base.
            if (structure->hasIndexingHeader(badObject)) {
                IndexingHeader* header = butterfly->indexingHeader();
                reg2 |= saturate<16>(header->publicLength()) << nibbleShift(Reg2::butterflyPublicLength);
                reg2 |= saturate<16>(header->vectorLength()) << nibbleShift(Reg2::butterflyVectorLength);
            }

            updateDumpState(0x570e);

            // The offending slot itself and its immediate neighbors. Re-reading the slot now can
            // reveal a race (it was empty when getDirect saw it). A neighbor that is cell-tagged
            // but doesn't resolve to a live heap cell is a strong corruption signal, so classify
            // each neighbor.
            const char* slot = storageBase + offsetInOutOfLineStorage(offset) * 8;
            uint64_t offendingWord = 0;
            uint64_t prevWord = 0;
            uint64_t nextWord = 0;
            if (slot >= blockStart && slot + 8 <= blockEnd)
                memcpySpan(asMutableByteSpan(offendingWord), unsafeMakeSpan(reinterpret_cast<const uint8_t*>(slot), 8));
            if (slot - 8 >= blockStart)
                memcpySpan(asMutableByteSpan(prevWord), unsafeMakeSpan(reinterpret_cast<const uint8_t*>(slot - 8), 8));
            if (slot + 16 <= blockEnd)
                memcpySpan(asMutableByteSpan(nextWord), unsafeMakeSpan(reinterpret_cast<const uint8_t*>(slot + 8), 8));
            reg5 |= classifySlotWord(prevWord) << nibbleShift(Reg5::previousSlotClassification);
            reg5 |= classifySlotWord(nextWord) << nibbleShift(Reg5::nextSlotClassification);
            updateDumpState(0x570f);
        } else
            updateDumpState(0x5710);
    } else if (isInlineOffset(offset)) {
        // Inline offending slot: read it and its neighbors, bounded by the containing cell.
        auto* slot = reinterpret_cast<const char*>(badObject->inlineStorage()) + offsetInInlineStorage(offset) * 8;
        auto* cellStart = reinterpret_cast<const char*>(badObject);
        const char* cellEnd = cellStart + badObject->cellSize();
        uint64_t offendingWord = 0;
        uint64_t prevWord = 0;
        uint64_t nextWord = 0;
        if (slot >= cellStart && slot + 8 <= cellEnd)
            memcpySpan(asMutableByteSpan(offendingWord), unsafeMakeSpan(reinterpret_cast<const uint8_t*>(slot), 8));
        if (slot - 8 >= cellStart)
            memcpySpan(asMutableByteSpan(prevWord), unsafeMakeSpan(reinterpret_cast<const uint8_t*>(slot - 8), 8));
        if (slot + 16 <= cellEnd)
            memcpySpan(asMutableByteSpan(nextWord), unsafeMakeSpan(reinterpret_cast<const uint8_t*>(slot + 8), 8));
        reg5 |= classifySlotWord(prevWord) << nibbleShift(Reg5::previousSlotClassification);
        reg5 |= classifySlotWord(nextWord) << nibbleShift(Reg5::nextSlotClassification);
        updateDumpState(0x5711);
    }

    updateDumpState(0x5712);

    UNUSED_PARAM(propertyName);
    UNUSED_PARAM(attributes);
    WTFCrashWithInfo(line, filename, function_name, 0x100900d0ff5e7bad, reg1, reg2, reg3, reg4, reg5, reg6);
#else
    UNUSED_PARAM(structure);
    UNUSED_PARAM(propertyName);
    UNUSED_PARAM(offset);
    UNUSED_PARAM(bottomOfChain);
    UNUSED_PARAM(previousInChain);
    UNUSED_PARAM(attributes);
    WTFCrashWithInfo(line, filename, function_name, 0x100900d0ff5e7bad);
#endif
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
