/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedObject.h"

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "AOTTypedType.h"
#include "AbstractModuleRecord.h"
#include "AsyncFunctionPrototype.h"
#include "AsyncGeneratorFunctionPrototype.h"
#include "AsyncGeneratorPrototype.h"
#include "FunctionPrototype.h"
#include "GeneratorFunctionPrototype.h"
#include "GeneratorPrototype.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "IteratorOperations.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "LLIntThunks.h"
#include "Microtask.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
#include "PropertyNameArray.h"
#include "ProtoCallFrameInlines.h"
#include "ProxyObject.h"
#include "TopExceptionScope.h"
#include "TypeError.h"
#include "WeakGCMapInlines.h"
#include <unicode/utf16.h>
#include <wtf/BitVector.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>

namespace JSC { namespace AOT { namespace Typed {

const ClassInfo Object::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Object) };

Structure* Object::createStructure(VM& vm, Instance& instance, JSValue prototype, const LayoutData* layout)
{
    // As of a proxy: it comes to have indexed properties without a transition, and nothing is to give it indexed storage.
    Structure* structure = isObjectLayout(layout->kind)
        ? Structure::create(vm, instance.globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info(), NonArray | MayHaveIndexedAccessors, 0)
        : Structure::create(vm, nullptr, jsNull(), TypeInfo(CellType, StructureFlags), info(), NonArray, 0);
    structure->setCompiledLayout(layout);
    structure->setAOTInstance(&instance);
    // `Object.create(object)`: becoming a prototype is a transition otherwise, and these have none.
    structure->setMayBePrototype(true);
    // What goes past the method table (putDirect(), setPrivateBrand(), ensureArrayStorage()) throws a TypeError where it would make a transition.
    structure->setHasImmutableShape(true);
    return structure;
}

Object* Object::create(VM& vm, Structure* structure, size_t size, uint32_t orderWord)
{
    const LayoutData& layout = *structure->compiledLayout();
    RELEASE_ASSERT(size >= layout.size);
    Object* object = new (NotNull, allocateCell<Object>(vm, size)) Object(vm, structure);
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), size - sizeof(JSCell) });
    for (uint32_t i = 0; i < layout.numberOfFields; ++i)
        object->clear(layout.fields()[i]);
    if (isObjectLayout(layout.kind)) {
        object->at<uint32_t>(offsetOfOrderWord) = orderWord;
        object->at<uint32_t>(offsetOfShapeWord) = shapeWordOf(layout.shape);
    }
    vm.mutatorFence();
    return object;
}

Object* Object::createWithAllFields(VM& vm, Structure* structure, size_t size, uint32_t orderWord)
{
    Object* object = create(vm, structure, size, orderWord);
    const LayoutData& layout = object->layout();
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = layout.fields()[i];
        // What cannot hold `undefined` is stored to before anyone looks.
        bool isUndefined = field.flags & fieldAllowsUndefined;
        switch (field.store) {
        case Store::JSValue:
            object->at<EncodedJSValue>(field.offset) = JSValue::encode(isUndefined ? jsUndefined() : JSValue());
            break;
        case Store::OptionalInt64:
            object->at<int64_t>(field.offset) = isUndefined ? undefinedInteger : 0;
            break;
        case Store::OptionalDouble:
            object->at<uint64_t>(field.offset) = isUndefined ? undefinedDouble : 0;
            break;
        default:
            break;
        }
    }
    return object;
}

const LayoutData& Object::layout() const
{
    return *structure()->compiledLayout();
}

State& Object::state() const
{
    return *structure()->aotInstance()->typed;
}

// What is kept at the address that way. Empty: nothing.
static JSValue readAs(Store store, const void* address)
{
    auto as = [&]<typename T>(T) {
        return *static_cast<const T*>(address);
    };
    switch (store) {
    case Store::Boolean:
        return jsBoolean(as(uint8_t()));
    case Store::Uint8:
        return jsNumber(as(uint8_t()));
    case Store::Int8:
        return jsNumber(as(int8_t()));
    case Store::Uint16:
        return jsNumber(as(uint16_t()));
    case Store::Int16:
        return jsNumber(as(int16_t()));
    case Store::Int32:
        return jsNumber(as(int32_t()));
    case Store::Uint32:
        return jsNumber(as(uint32_t()));
    case Store::Int64:
        return jsNumber(static_cast<double>(as(int64_t())));
    case Store::Float:
        return jsNumber(purifyNaN(as(float())));
    case Store::Double:
        return jsNumber(purifyNaN(as(double())));
    case Store::Cell:
    case Store::JSValue:
        return JSValue::decode(as(EncodedJSValue()));
    case Store::OptionalInt64:
        switch (int64_t integer = as(int64_t())) {
        case undefinedInteger:
            return jsUndefined();
        case nullInteger:
            return jsNull();
        case absentInteger:
            return { };
        default:
            return jsNumber(static_cast<double>(integer));
        }
    case Store::OptionalDouble:
        switch (as(uint64_t())) {
        case undefinedDouble:
            return jsUndefined();
        case nullDouble:
            return jsNull();
        case absentDouble:
            return { };
        default:
            return jsNumber(purifyNaN(as(double())));
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// False: the value cannot be kept that way. Whoever owns the memory sees to the write barrier.
static bool storeAs(Store store, void* address, JSValue value)
{
    auto keep = [&](auto bits) {
        *static_cast<decltype(bits)*>(address) = bits;
        return true;
    };
    auto integer = [&]<typename T>(int64_t low, int64_t high, T) {
        if (!value.isNumber())
            return false;
        double number = value.asNumber();
        if (!(number >= static_cast<double>(low) && number <= static_cast<double>(high)) || number != std::trunc(number))
            return false;
        return keep(static_cast<T>(static_cast<int64_t>(number)));
    };
    auto marker = [&](auto undefinedMarker, auto nullMarker) {
        return value.isUndefinedOrNull() && keep(value.isUndefined() ? undefinedMarker : nullMarker);
    };
    switch (store) {
    case Store::Boolean:
        return value.isBoolean() && keep(static_cast<uint8_t>(value.asBoolean()));
    case Store::Uint8:
        return integer(0, UINT8_MAX, uint8_t());
    case Store::Int8:
        return integer(INT8_MIN, INT8_MAX, int8_t());
    case Store::Uint16:
        return integer(0, UINT16_MAX, uint16_t());
    case Store::Int16:
        return integer(INT16_MIN, INT16_MAX, int16_t());
    case Store::Int32:
        return integer(INT32_MIN, INT32_MAX, int32_t());
    case Store::Uint32:
        return integer(0, UINT32_MAX, uint32_t());
    case Store::Int64:
        return integer(-maxExactInteger, maxExactInteger, int64_t());
    case Store::Float: {
        if (!value.isNumber())
            return false;
        double number = value.asNumber();
        if (number == number && static_cast<double>(static_cast<float>(number)) != number)
            return false;
        return keep(static_cast<float>(number));
    }
    case Store::Double:
        return value.isNumber() && keep(value.asNumber());
    case Store::OptionalInt64:
        return marker(undefinedInteger, nullInteger) || integer(-maxExactInteger, maxExactInteger, int64_t());
    case Store::OptionalDouble:
        return marker(undefinedDouble, nullDouble) || (value.isNumber() && keep(purifyNaN(value.asNumber())));
    case Store::Cell:
        if (!value.isCell())
            return false;
        [[fallthrough]];
    case Store::JSValue:
        return keep(JSValue::encode(value));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue Object::read(const FieldData& field) const
{
    return readAs(field.store, &at<uint8_t>(field.offset));
}

bool Object::write(VM& vm, const FieldData& field, JSValue value)
{
    // Compiled code takes it that such a field keeps the value the object was born with.
    if (field.flags & fieldIsImmutable)
        return false;
    State& state = this->state();
    return state.fits(state.typeOf(field), value) && store(vm, field, value);
}

bool Object::assign(JSGlobalObject* globalObject, const FieldData& field, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    String failure;
    JSValue held = field.flags & fieldIsImmutable ? JSValue() : state.holdToTypeOf(globalObject, field, value, failure);
    RETURN_IF_EXCEPTION(scope, false);
    if (!held || !store(vm, field, held)) {
        throwTypeError(globalObject, scope, state.describeRefusedStore(makeString("property '"_s, StringView(state.name(vm, field.name).impl()), "' of an object of type "_s, state.nameOf(layout())), field, value, failure));
        return false;
    }
    return true;
}

bool Object::store(VM& vm, const FieldData& field, JSValue value)
{
    if (!storeAs(field.store, &at<uint8_t>(field.offset), value))
        return false;
    if (holdsCells(field.store))
        vm.writeBarrier(this, value);
    return true;
}

void Object::clear(const FieldData& field)
{
    switch (field.store) {
    case Store::Cell:
    case Store::JSValue:
        at<EncodedJSValue>(field.offset) = JSValue::encode(JSValue());
        return;
    case Store::OptionalInt64:
        at<int64_t>(field.offset) = absentInteger;
        return;
    case Store::OptionalDouble:
        at<uint64_t>(field.offset) = absentDouble;
        return;
    default:
        return;
    }
}

template<typename Visitor>
void Object::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSCell::visitChildren(cell, visitor);
    // While specialize() is at work this may be the old layout or the new one. The fields of either are fit to be visited.
    const LayoutData& layout = object->layout();
    if (isObjectLayout(layout.kind) && object->orderWord() & orderWordHasExtraProperties) {
        if (JSObject* extra = object->state().extraProperties.getConcurrently(object))
            visitor.appendUnbarriered(extra);
    }
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = layout.fields()[i];
        if (holdsCells(field.store))
            visitor.appendUnbarriered(JSValue::decode(object->at<EncodedJSValue>(field.offset)));
    }
}

DEFINE_VISIT_CHILDREN(Object);

size_t Object::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

void Object::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSCell::analyzeHeap(cell, analyzer);
    State& state = object->state();
    const LayoutData& layout = object->layout();
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = layout.fields()[i];
        if (!holdsCells(field.store))
            continue;
        JSValue value = JSValue::decode(object->at<EncodedJSValue>(field.offset));
        // The collector cannot make an identifier of a name that nothing has asked for yet.
        const Identifier& name = state.names[field.name];
        if (!value || !value.isCell() || name.isNull())
            continue;
        if (isObjectLayout(layout.kind))
            analyzer.analyzePropertyNameEdge(object, value.asCell(), name.impl());
        else
            analyzer.analyzeVariableNameEdge(object, value.asCell(), name.impl());
    }
}

ExtraPropertiesTable::ExtraPropertiesTable(VM& vm)
    : m_vm(vm)
{
    vm.heap.registerWeakGCHashTable(this);
}

ExtraPropertiesTable::~ExtraPropertiesTable()
{
    m_vm.heap.unregisterWeakGCHashTable(this);
}

JSObject* ExtraPropertiesTable::getConcurrently(const Object* object)
{
    Locker locker { m_lock };
    return get(object);
}

void ExtraPropertiesTable::set(Object* object, JSObject* extra)
{
    {
        Locker locker { m_lock };
        m_map.set(object, extra);
    }
    markDirty(m_vm);
}

void ExtraPropertiesTable::remove(Object* object)
{
    Locker locker { m_lock };
    m_map.remove(object);
}

void ExtraPropertiesTable::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope)
{
    Locker locker { m_lock };
    m_map.removeIf([&](auto& entry) {
        return !vm.heap.isMarked(entry.key);
    });
}

JSObject* Object::extraProperties() const
{
    return orderWord() & orderWordHasExtraProperties ? state().extraProperties.get(this) : nullptr;
}

JSObject* Object::ensureExtraProperties(VM& vm)
{
    if (JSObject* existing = extraProperties())
        return existing;
    JSObject* extra = constructEmptyObject(vm, structure()->realm()->nullPrototypeObjectStructure());
    state().extraProperties.set(this, extra);
    at<uint32_t>(offsetOfOrderWord) |= orderWordHasExtraProperties;
    vm.writeBarrier(this, extra);
    return extra;
}

// Array.prototype.concat() takes it from the Structure that what inherits from Object.prototype has no such property.
void Object::willAddExtraProperty(VM& vm, PropertyName name)
{
    if (name == vm.propertyNames->isConcatSpreadableSymbol) [[unlikely]]
        realm()->arrayIsConcatSpreadableWatchpointSet().fireAll(vm, "An object of the program has Symbol.isConcatSpreadable");
}

void Object::dropExtraPropertiesIfEmpty(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra || hasIndexedProperties(extra->indexingType()))
        return;
    bool isEmpty = true;
    extra->structure()->forEachProperty(vm, [&](const auto&) {
        isEmpty = false;
        return false;
    });
    if (!isEmpty)
        return;
    at<uint32_t>(offsetOfOrderWord) &= ~orderWordHasExtraProperties;
    state().extraProperties.remove(this);
}

const FieldData* Object::fieldNamed(State& state, PropertyName name) const
{
    if (name.isSymbol() && !name.isPrivateName())
        return nullptr;
    return state.field(*state.instance.vm, layout(), name.uid());
}

void Object::forEachFieldInKeyOrder(State& state, const Invocable<void(uint16_t)> auto& functor) const
{
    const LayoutData& layout = this->layout();
    uint32_t word = orderWord();
    if (orderOf(word) != keysAreInLayoutOrder) {
        for (uint16_t index : state.fieldsInOrder(layout, orderOf(word)).first(countOf(word)))
            functor(index);
        return;
    }
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        if (read(layout.fields()[i]))
            functor(i);
    }
}

void Object::didCreateField(State& state, const FieldData& field)
{
    setOrderAndCount(state.orderAfterCreating(layout(), orderWord(), indexOf(field)));
}

// Nothing is inherited, and no proxy is asked. What is not in a field or of the class is kept as a property by the name: read-only if a method.
Object::PrivateElement Object::findPrivateElement(VM& vm, JSObject* object, PropertyName name)
{
    PrivateElement element;
    auto findIn = [&](JSObject* holder) {
        unsigned attributes;
        JSValue found = holder->getDirect(vm, name, attributes);
        if (!found)
            return false;
        if (attributes & PropertyAttribute::Accessor) {
            auto* accessor = uncheckedDowncast<GetterSetter>(found);
            element.kind = PrivateElement::Kind::Accessor;
            element.getter = accessor->isGetterNull() ? nullptr : accessor->getter();
            element.setter = accessor->isSetterNull() ? nullptr : accessor->setter();
            return true;
        }
        element.kind = attributes & PropertyAttribute::ReadOnly ? PrivateElement::Kind::Method : PrivateElement::Kind::Field;
        element.value = found;
        return true;
    };
    auto* compiled = dynamicDowncast<Object>(object);
    if (!compiled) {
        findIn(object);
        return element;
    }
    State& state = compiled->state();
    if (const FieldData* field = compiled->fieldNamed(state, name)) {
        element.value = compiled->read(*field);
        if (element.value)
            element.kind = PrivateElement::Kind::Field;
        return element;
    }
    if (state.findPrivateMethod(vm, compiled->layout(), name.uid(), element))
        return element;
    if (JSObject* extra = compiled->extraProperties(); extra && findIn(extra))
        return element;
    // Of a class that is not evaluated once they are functions of the evaluation the object is of: its prototype object keeps them.
    for (JSValue next = compiled->getPrototypeDirect(); next.isObject() && !findIn(asObject(next)); next = asObject(next)->getPrototypeDirect()) { }
    return element;
}

// Compiled code calls a method of a class by the shape of the object, whatever the object has by the name, unless the program is seen to
// replace the method. The one thing the object may have is what makes no difference to that: the method it inherits, bound to the
// object itself. `this.stop = this.stop.bind(this)`.
static bool isInheritedMethodBoundTo(VM& vm, Object* object, PropertyName name, JSValue value)
{
    JSObject* target = nullptr;
    for (auto* bound = dynamicDowncast<JSBoundFunction>(value); bound; bound = dynamicDowncast<JSBoundFunction>(target)) {
        if (bound->boundThis() != JSValue(object) || bound->boundArgsLength())
            return false;
        target = bound->targetFunction();
    }
    if (!target)
        return false;
    for (JSValue next = object->getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        if (JSValue inherited = asObject(next)->getDirect(vm, name))
            return inherited == JSValue(target);
    }
    return false;
}

static void throwIsMethodOfClass(JSGlobalObject* globalObject, ThrowScope& scope, State& state, const LayoutData& layout, PropertyName name)
{
    throwTypeError(globalObject, scope, makeString("Cannot define property '"_s, StringView(name.uid()), "' on an object of class "_s, state.nameOf(layout), ": it is a method of the class that the compiler has not seen the program replace, and the value is not that method bound to the object"_s));
}

static unsigned attributesOfFields(const Object* object)
{
    if (object->isFrozen())
        return PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    return static_cast<unsigned>(object->isSealed() ? PropertyAttribute::DontDelete : PropertyAttribute::None);
}

bool Object::getOwnPropertySlot(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    if (const FieldData* field = object->fieldNamed(object->state(), name)) {
        // A field that is absent is no property: the search goes on along the prototype chain.
        JSValue value = object->read(*field);
        if (!value)
            return false;
        slot.setValue(object, attributesOfFields(object), value);
        return true;
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return false;
    bool found = extra->methodTable()->getOwnPropertySlot(extra, globalObject, name, slot);
    slot.disableCaching();
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

// Integer indices, the fields in the order of their creation, the other strings, the symbols.
void Object::getOwnPropertyNames(JSObject* cell, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    State& state = object->state();
    JSObject* extra = object->extraProperties();
    if (extra)
        extra->getOwnIndexedPropertyNames(globalObject, names, mode);
    if (names.includeStringProperties()) {
        const FieldData* fields = object->layout().fields();
        object->forEachFieldInKeyOrder(state, [&](uint16_t index) {
            if (!state.isPrivate(fields[index]))
                names.add(state.name(vm, fields[index].name));
        });
    }
    if (extra)
        extra->getOwnNonIndexPropertyNames(globalObject, names, mode);
}

// Whether [[Set]] of the object with another receiver may do anything but have the receiver define the property.
static bool mayInterceptPut(VM& vm, JSObject* object, PropertyName name)
{
    if (name == vm.propertyNames->underscoreProto)
        return true;
    while (true) {
        Structure* structure = object->structure();
        if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->typeInfo().overridesGetPrototype() || structure->typeInfo().overridesPut())
            return true;
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return false;
        object = asObject(prototype);
    }
}

// OrdinarySet step by step: with another receiver, as when the object is on the prototype chain of the receiver, and with an index. What
// the object has not got is for [[Set]] of its parent. ordinarySetSlow() walks the chain by itself, and takes for read-only what
// inheritors may override (Structure::inheritorsMayOverrideReadOnlyProperties()).
static bool setStepByStep(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyDescriptor ownDescriptor;
    bool isFound = object->getOwnPropertyDescriptor(globalObject, name, ownDescriptor);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isFound) {
        JSValue prototype = object->getPrototypeDirect();
        if (prototype.isObject())
            RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
        ownDescriptor = PropertyDescriptor(jsUndefined(), static_cast<unsigned>(PropertyAttribute::None));
    }
    RELEASE_AND_RETURN(scope, ordinarySetWithOwnDescriptor(globalObject, object, name, value, slot.thisValue(), WTF::move(ownDescriptor), slot.isStrictMode()));
}

// OrdinarySet.
bool Object::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (slot.thisValue() != JSValue(object) || parseIndex(name)) [[unlikely]]
        RELEASE_AND_RETURN(scope, setStepByStep(globalObject, object, name, value, slot));
    State& state = object->state();
    if (const FieldData* field = object->fieldNamed(state, name)) {
        // Also of a field that is absent, whatever is inherited: a compiled store does not ask the prototype chain either. `#name` is no property.
        if (object->isFrozen() && !name.isPrivateName())
            return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
        if (object->read(*field))
            RELEASE_AND_RETURN(scope, object->assign(globalObject, *field, value));
    } else if (JSObject* extra = object->extraProperties()) {
        unsigned attributes;
        if (JSValue existing = extra->getDirect(vm, name, attributes)) {
            if (attributes & PropertyAttribute::Accessor)
                RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(existing)->callSetter(globalObject, object, value, slot.isStrictMode()));
            if (attributes & PropertyAttribute::ReadOnly)
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            if (state.hasFixedMethod(object->layout(), name.uid()) && !isInheritedMethodBoundTo(vm, object, name, value)) {
                throwIsMethodOfClass(globalObject, scope, state, object->layout(), name);
                return false;
            }
            extra->putDirect(vm, name, value, attributes);
            return true;
        }
    }
    // It has no such property, so its parent is asked, where a setter or a read-only property may take the store.
    JSValue prototype = object->getPrototypeDirect();
    if (prototype.isObject() && !name.isPrivateName() && mayInterceptPut(vm, asObject(prototype), name))
        RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
    RELEASE_AND_RETURN(scope, defineOwnProperty(object, globalObject, name, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), slot.isStrictMode()));
}

void Object::putExtraWithoutRunningCode(JSGlobalObject* globalObject, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    ASSERT(!fieldNamed(state, name));
    auto refuse = [&](ASCIILiteral why) {
        throwTypeError(globalObject, scope, makeString("Cannot store to property '"_s, StringView(name.uid()), "' of an object of type "_s, state.nameOf(layout()), ": "_s, why));
    };
    unsigned attributes;
    JSObject* extra = extraProperties();
    if (extra && extra->getDirect(vm, name, attributes)) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return refuse("it is an accessor"_s);
        if (attributes & PropertyAttribute::ReadOnly)
            return refuse("it is read-only"_s);
        extra->putDirect(vm, name, value, attributes);
        return;
    }
    for (JSValue next = getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        JSObject* holder = asObject(next);
        Structure* structure = holder->structure();
        TypeInfo typeInfo = structure->typeInfo();
        if (typeInfo.overridesGetOwnPropertySlot() || typeInfo.overridesPut() || typeInfo.overridesGetPrototype() || holder->hasNonReifiedStaticProperties())
            return refuse("an object on its prototype chain may take the store"_s);
        if (!holder->getDirect(vm, name, attributes))
            continue;
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return refuse("it inherits an accessor"_s);
        if (attributes & PropertyAttribute::ReadOnly && !structure->inheritorsMayOverrideReadOnlyProperties())
            return refuse("it inherits a read-only property"_s);
        break;
    }
    scope.release();
    defineOwnProperty(this, globalObject, name, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), true);
}

bool Object::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    PutPropertySlot slot(cell, shouldThrow);
    return setStepByStep(globalObject, uncheckedDowncast<Object>(cell), Identifier::from(globalObject->vm(), index), value, slot);
}

bool Object::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    if (const FieldData* field = object->fieldNamed(state, name)) {
        if (!object->read(*field))
            return true;
        if (object->isSealed())
            return false;
        const LayoutData& layout = object->layout();
        if (!(field->flags & fieldIsOptional)) {
            throwTypeError(globalObject, scope, makeString("Cannot delete property '"_s, StringView(name.uid()), "' of an object of type "_s, state.nameOf(layout), ": it is not optional"_s));
            return false;
        }
        RELEASE_ASSERT(state.mayBeAbsent(*field));
        if (orderOf(object->orderWord()) != keysAreInLayoutOrder) {
            Vector<uint16_t, 16> remaining;
            object->forEachFieldInKeyOrder(state, [&](uint16_t index) {
                if (index != object->indexOf(*field))
                    remaining.append(index);
            });
            object->setOrderAndCount(state.orderStartingWith(layout, remaining.span()));
        }
        object->clear(*field);
        return true;
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    bool isDeleted = extra->methodTable()->deleteProperty(extra, globalObject, name, slot);
    slot.disableCaching();
    if (isDeleted)
        object->dropExtraPropertiesIfEmpty(vm);
    return isDeleted;
}

bool Object::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    JSObject* extra = uncheckedDowncast<Object>(cell)->extraProperties();
    return !extra || extra->methodTable()->deletePropertyByIndex(extra, globalObject, index);
}

bool Object::defineOwnProperty(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    const LayoutData& layout = object->layout();
    const FieldData* field = object->fieldNamed(state, name);
    if (!field) {
        JSObject* extra = object->extraProperties();
        bool exists = extra && extra->hasOwnProperty(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        // A store to a hole of what inherits from the object looks for setters and read-only elements in indexed storage only.
        if (parseIndex(name) && !object->isFrozen() && (descriptor.isAccessorDescriptor() || !(descriptor.writablePresent() ? descriptor.writable() : exists))) {
            throwTypeError(globalObject, scope, makeString("Cannot define property '"_s, StringView(name.uid()), "' on an object of type "_s, state.nameOf(layout), ": an indexed property of it is a writable data property"_s));
            return false;
        }
        if (state.hasFixedMethod(layout, name.uid()) && (descriptor.isAccessorDescriptor() || (descriptor.value() ? !isInheritedMethodBoundTo(vm, object, name, descriptor.value()) : !exists))) {
            throwIsMethodOfClass(globalObject, scope, state, layout, name);
            return false;
        }
        if (!exists) {
            if (object->isNotExtensible())
                return typeError(globalObject, scope, shouldThrow, NonExtensibleObjectPropertyDefineError);
            object->willAddExtraProperty(vm, name);
            extra = object->ensureExtraProperties(vm);
        }
        RELEASE_AND_RETURN(scope, extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow));
    }

    // ValidateAndApplyPropertyDescriptor. What a field holds is a data property, writable, enumerable and configurable unless the object is frozen.
    JSValue current = object->read(*field);
    unsigned currentAttributes = attributesOfFields(object);
    PropertyDescriptor currentDescriptor;
    if (current)
        currentDescriptor.setDescriptor(current, currentAttributes);
    bool isValid = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, !object->isNotExtensible(), descriptor, !!current, currentDescriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isValid)
        return false;
    unsigned attributes = current ? descriptor.attributesOverridingCurrent(currentDescriptor) : descriptor.attributes();
    if (attributes != currentAttributes) {
        throwTypeError(globalObject, scope, makeString("Cannot redefine property '"_s, StringView(name.uid()), "' of an object of type "_s, state.nameOf(layout), ": it stays a writable, enumerable and configurable data property"_s));
        return false;
    }
    if (object->isFrozen())
        return true;
    // What is there stays as it is.
    if (current && !descriptor.value())
        return true;
    bool isStored = object->assign(globalObject, *field, descriptor.value() ? descriptor.value() : jsUndefined());
    RETURN_IF_EXCEPTION(scope, false);
    if (isStored && !current)
        object->didCreateField(state, *field);
    return isStored;
}

bool Object::setIntegrity(JSGlobalObject* globalObject, Integrity integrity)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t bits = orderWordIsNotExtensible;
    if (integrity != Integrity::NotExtensible)
        bits |= orderWordIsSealed;
    if (integrity == Integrity::Frozen)
        bits |= orderWordIsFrozen;
    if ((orderWord() & bits) == bits)
        return true;
    if (!layout().mayBeFrozen) {
        throwTypeError(globalObject, scope, makeString("Cannot freeze, seal or prevent the extension of an object of type "_s, state().nameOf(layout()), ": the program is not seen to do that to objects of that type"_s));
        return false;
    }
    if (JSObject* extra = extraProperties()) {
        switch (integrity) {
        case Integrity::NotExtensible:
            extra->methodTable()->preventExtensions(extra, globalObject);
            break;
        case Integrity::Sealed:
            objectConstructorSeal(globalObject, extra);
            break;
        case Integrity::Frozen:
            objectConstructorFreeze(globalObject, extra);
            break;
        }
        RETURN_IF_EXCEPTION(scope, false);
    }
    at<uint32_t>(offsetOfOrderWord) |= bits;
    return true;
}

bool Object::preventExtensions(JSObject* cell, JSGlobalObject* globalObject)
{
    return uncheckedDowncast<Object>(cell)->setIntegrity(globalObject, Integrity::NotExtensible);
}

bool Object::isExtensible(JSObject* cell, JSGlobalObject*)
{
    return !uncheckedDowncast<Object>(cell)->isNotExtensible();
}

bool Object::setPrototype(JSObject* cell, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (object->getPrototypeDirect() == prototype)
        return true;
    return typeError(globalObject, scope, shouldThrow, makeString("Cannot change the prototype of an object of type "_s, object->state().nameOf(object->layout())));
}

void Object::setPrototypeAtBirth(VM& vm, JSObject* prototype)
{
    if (getPrototypeDirect() != JSValue(prototype))
        JSCell::setStructure(vm, state().structureFor(vm, layout().shape, prototype));
}

void Object::specialize(VM& vm, State& state, uint32_t shape)
{
    const LayoutData& from = layout();
    const LayoutData& to = state.layoutOfShape(shape);
    RELEASE_ASSERT(cellSize() >= to.size);
    Structure* structure = state.structureFor(vm, shape, getPrototypeDirect());
    BitVector isKept;
    Vector<uint16_t, 16> indexInNewLayout;
    for (uint32_t i = 0; i < from.numberOfFields; ++i) {
        const FieldData& old = from.fields()[i];
        const FieldData* field = state.field(vm, to, state.name(vm, old.name).impl());
        RELEASE_ASSERT(field && field->offset == old.offset && field->store == old.store && field->type == old.type);
        indexInNewLayout.append(field - to.fields());
        isKept.set(field - to.fields());
    }
    uint32_t orderAndCount = orderWordOf(keysAreInLayoutOrder, 0);
    if (orderOf(orderWord()) != keysAreInLayoutOrder) {
        Vector<uint16_t, 16> order;
        forEachFieldInKeyOrder(state, [&](uint16_t index) {
            order.append(indexInNewLayout[index]);
        });
        orderAndCount = state.orderStartingWith(to, order.span());
    }
    for (uint32_t i = 0; i < to.numberOfFields; ++i) {
        if (isKept.get(i))
            continue;
        RELEASE_ASSERT(state.mayBeAbsent(to.fields()[i]));
        clear(to.fields()[i]);
    }
    // The collector goes by the Structure, and may be marking.
    WTF::storeStoreFence();
    JSCell::setStructure(vm, structure);
    setOrderAndCount(orderAndCount);
    at<uint32_t>(offsetOfShapeWord) = shapeWordOf(shape);
}

const ClassInfo Closure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Closure) };

Structure* Closure::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

Closure* Closure::create(VM& vm, Structure* structure, uint64_t entry, JSCell* environment, uint32_t function)
{
    Closure* closure = new (NotNull, allocateCell<Closure>(vm)) Closure(vm, structure, entry, environment, function);
    closure->finishCreation(vm);
    return closure;
}

template<typename Visitor>
void Closure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Closure* closure = uncheckedDowncast<Closure>(cell);
    Base::visitChildren(cell, visitor);
    if (closure->m_environment)
        visitor.appendUnbarriered(closure->m_environment);
}

DEFINE_VISIT_CHILDREN(Closure);

static JSC_DECLARE_HOST_FUNCTION(callClosure);

JSC_DEFINE_HOST_FUNCTION(callClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    ProtoCallFrame protoCallFrame;
    protoCallFrame.init(nullptr, globalObject, callFrame->jsCallee(), callFrame->thisValue(), nullptr, callFrame->argumentCountIncludingThis(), reinterpret_cast<EncodedJSValue*>(callFrame->addressOfArgumentsStart()));
    return vmEntryToJavaScript(closureEntrypoint().taggedPtr(), &vm, &protoCallFrame);
}

static JSC_DECLARE_HOST_FUNCTION(callClassConstructor);
static JSC_DECLARE_HOST_FUNCTION(constructClosure);

JSC_DEFINE_HOST_FUNCTION(callClassConstructor, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Cannot call a class constructor without |new|"_s);
}

// What the function is handed when it is constructed: a constructor that asks for it gets `new.target` first.
static void appendArgumentsOfConstruct(MarkedArgumentBuffer& all, const Closure* closure, JSValue newTarget, const ArgList& arguments)
{
    if (const LayoutData* ofClass = closure->classData(); ofClass && ofClass->constructorTakesNewTarget)
        all.append(newTarget);
    for (unsigned i = 0; i < arguments.size(); ++i)
        all.append(arguments.at(i));
}

JSC_DEFINE_HOST_FUNCTION(constructClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* closure = uncheckedDowncast<Closure>(callFrame->jsCallee());
    JSObject* newTarget = asObject(callFrame->newTarget());
    JSValue thisValue = closure->structure()->aotInstance()->typed->createThis(globalObject, closure, newTarget);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    appendArgumentsOfConstruct(arguments, closure, newTarget, ArgList(callFrame));
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    ProtoCallFrame protoCallFrame;
    protoCallFrame.init(nullptr, globalObject, closure, thisValue, nullptr, arguments.size() + 1, arguments.data());
    JSValue result = JSValue::decode(vmEntryToJavaScript(closureEntrypoint().taggedPtr(), &vm, &protoCallFrame));
    RETURN_IF_EXCEPTION(scope, { });
    if (!result.isObject())
        result = thisValue;
    // Under a foreign root the constructor hands back what that has made.
    RELEASE_ASSERT(result.isObject());
    return JSValue::encode(result);
}

const ClassInfo Generator::s_info = { "Generator"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Generator) };

Structure* Generator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

Generator* Generator::create(VM& vm, Structure* structure, Closure* resumeClosure)
{
    Generator* generator = new (NotNull, allocateCell<Generator>(vm)) Generator(vm, structure, resumeClosure);
    generator->finishCreation(vm);
    return generator;
}

template<typename Visitor>
void Generator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitor.appendUnbarriered(uncheckedDowncast<Generator>(cell)->m_resumeClosure);
}

DEFINE_VISIT_CHILDREN(Generator);

JSValue Generator::resume(JSGlobalObject* globalObject, ResumeMode mode, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_isRunning) {
        throwTypeError(globalObject, scope, "Generator is executing"_s);
        return { };
    }
    Object* frame = uncheckedDowncast<Object>(m_resumeClosure->environment());
    const FieldData* fields = frame->layout().fields();
    auto complete = [&] {
        bool didWrite = frame->write(vm, fields[CoroutineFrame::stateField], jsNumber(CoroutineFrame::completedState));
        RELEASE_ASSERT(didWrite);
    };
    int32_t state = frame->read(fields[CoroutineFrame::stateField]).asInt32();
    // A generator that has not started completes without running: there is no `try` around its first instruction.
    if (!state && mode != ResumeMode::Next) {
        complete();
        state = CoroutineFrame::completedState;
    }
    if (state == CoroutineFrame::completedState) {
        if (mode == ResumeMode::Throw) {
            throwException(globalObject, scope, value);
            return { };
        }
        return createIteratorResultObject(globalObject, mode == ResumeMode::Return ? value : jsUndefined(), true);
    }
    bool didWrite = frame->write(vm, fields[CoroutineFrame::resumeValueField], value);
    RELEASE_ASSERT(didWrite);
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(static_cast<int32_t>(mode)));
    m_isRunning = true;
    JSValue result = call(globalObject, m_resumeClosure, Closure::getCallData(m_resumeClosure), jsUndefined(), arguments);
    m_isRunning = false;
    if (scope.exception()) [[unlikely]] {
        complete();
        return { };
    }
    bool isDone = frame->read(fields[CoroutineFrame::stateField]).asInt32() == CoroutineFrame::completedState;
    return createIteratorResultObject(globalObject, result, isDone);
}

const ClassInfo AsyncGenerator::s_info = { "AsyncGenerator"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(AsyncGenerator) };

Structure* AsyncGenerator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

AsyncGenerator* AsyncGenerator::create(VM& vm, JSGlobalObject* globalObject, Structure* structure, Closure* resumeClosure)
{
    JSArray* queue = constructEmptyArray(globalObject, nullptr);
    AsyncGenerator* generator = new (NotNull, allocateCell<AsyncGenerator>(vm)) AsyncGenerator(vm, structure, resumeClosure, queue);
    generator->finishCreation(vm);
    return generator;
}

template<typename Visitor>
void AsyncGenerator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    auto* generator = uncheckedDowncast<AsyncGenerator>(cell);
    visitor.appendUnbarriered(generator->m_resumeClosure);
    visitor.appendUnbarriered(generator->m_queue);
}

DEFINE_VISIT_CHILDREN(AsyncGenerator);

Object* AsyncGenerator::frame() const
{
    return uncheckedDowncast<Object>(m_resumeClosure->environment());
}

JSPromise* AsyncGenerator::enqueue(JSGlobalObject* globalObject, Generator::ResumeMode mode, JSValue value)
{
    VM& vm = globalObject->vm();
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    m_queue->push(globalObject, jsNumber(static_cast<int32_t>(mode)));
    m_queue->push(globalObject, value);
    m_queue->push(globalObject, promise);
    if (!m_isRunningOrAwaiting)
        drain(globalObject);
    return promise;
}

void AsyncGenerator::settleFrontRequest(JSGlobalObject* globalObject, JSValue value, bool isDone, bool isRejected)
{
    VM& vm = globalObject->vm();
    auto* promise = uncheckedDowncast<JSPromise>(m_queue->getIndexQuickly(m_head + 2));
    m_head += 3;
    if (m_head == m_queue->length()) {
        m_queue->setLength(globalObject, 0);
        m_head = 0;
    }
    if (isRejected)
        promise->reject(vm, value);
    else
        promise->resolve(globalObject, vm, createIteratorResultObject(globalObject, value, isDone));
}

void AsyncGenerator::drain(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    const FieldData* fields = frame()->layout().fields();
    while (!m_isRunningOrAwaiting && m_head < m_queue->length()) {
        auto mode = static_cast<Generator::ResumeMode>(m_queue->getIndexQuickly(m_head).asInt32());
        JSValue value = m_queue->getIndexQuickly(m_head + 1);
        int32_t state = frame()->read(fields[CoroutineFrame::stateField]).asInt32();
        if (!state && mode != Generator::ResumeMode::Next) {
            bool didWrite = frame()->write(vm, fields[CoroutineFrame::stateField], jsNumber(CoroutineFrame::completedState));
            RELEASE_ASSERT(didWrite);
            state = CoroutineFrame::completedState;
        }
        if (state == CoroutineFrame::completedState) {
            switch (mode) {
            case Generator::ResumeMode::Next:
                settleFrontRequest(globalObject, jsUndefined(), true, false);
                continue;
            case Generator::ResumeMode::Throw:
                settleFrontRequest(globalObject, value, true, true);
                continue;
            case Generator::ResumeMode::Return:
                m_isRunningOrAwaiting = true;
                JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, value, InternalMicrotask::AOTAsyncFunctionResume, this);
                return;
            }
        }
        bool didWrite = frame()->write(vm, fields[CoroutineFrame::resumeValueField], value);
        RELEASE_ASSERT(didWrite);
        run(globalObject, static_cast<int32_t>(mode));
    }
}

void AsyncGenerator::run(JSGlobalObject* globalObject, int32_t mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    const FieldData* fields = frame()->layout().fields();
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(mode));
    m_isRunningOrAwaiting = true;
    m_isSuspendingAtAwait = false;
    JSValue result = call(globalObject, m_resumeClosure, Closure::getCallData(m_resumeClosure), jsUndefined(), arguments);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (!scope.clearExceptionExceptTermination())
            return;
        bool didWrite = frame()->write(vm, fields[CoroutineFrame::stateField], jsNumber(CoroutineFrame::completedState));
        RELEASE_ASSERT(didWrite);
        m_isRunningOrAwaiting = false;
        settleFrontRequest(globalObject, error, true, true);
        return;
    }
    if (m_isSuspendingAtAwait)
        return;
    m_isRunningOrAwaiting = false;
    settleFrontRequest(globalObject, result, frame()->read(fields[CoroutineFrame::stateField]).asInt32() == CoroutineFrame::completedState, false);
}

// What was awaited has settled.
static void storeResolution(JSGlobalObject* globalObject, Object* frame, JSValue resolution, bool isRejected)
{
    bool didWrite = frame->write(globalObject->vm(), frame->layout().fields()[isRejected ? CoroutineFrame::resumeErrorField : CoroutineFrame::resumeValueField], resolution);
    RELEASE_ASSERT(didWrite);
}

void AsyncGenerator::resumeFromAwait(JSGlobalObject* globalObject, JSValue resolution, bool isRejected)
{
    storeResolution(globalObject, frame(), resolution, isRejected);
    run(globalObject, isRejected);
    drain(globalObject);
}

void AsyncGenerator::didAwaitReturn(JSGlobalObject* globalObject, JSValue resolution, bool isRejected)
{
    m_isRunningOrAwaiting = false;
    settleFrontRequest(globalObject, resolution, true, isRejected);
    drain(globalObject);
}

// next(), return() or throw() for the generators of the program. Whatever else it is called on is for the function it takes the place of.
template<typename GeneratorType, Generator::ResumeMode mode>
static void installGeneratorMethod(JSGlobalObject* globalObject, JSObject* prototype, const Identifier& name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    // It makes a property of what is in the static table of the class.
    JSValue found = prototype->get(globalObject, name);
    scope.assertNoException();
    unsigned attributes;
    RELEASE_ASSERT(found.isCallable() && prototype->getDirect(vm, name, attributes) == found);
    if (found.inherits<JSNativeStdFunction>())
        return;
    JSObject* original = asObject(found);
    auto* function = JSNativeStdFunction::create(vm, globalObject, 1, name.string(), [original](JSGlobalObject* globalObject, CallFrame* callFrame) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto* generator = dynamicDowncast<GeneratorType>(callFrame->thisValue());
        if (!generator) [[unlikely]]
            RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, original, JSC::getCallData(original), callFrame->thisValue(), ArgList(callFrame))));
        if constexpr (std::is_same_v<GeneratorType, Generator>)
            RELEASE_AND_RETURN(scope, JSValue::encode(generator->resume(globalObject, mode, callFrame->argument(0))));
        else
            RELEASE_AND_RETURN(scope, JSValue::encode(generator->enqueue(globalObject, mode, callFrame->argument(0))));
    }, found);
    prototype->putDirect(vm, name, function, attributes);
}

void installGeneratorMethods(JSGlobalObject* globalObject)
{
    auto& names = *globalObject->vm().propertyNames;
    JSObject* prototype = globalObject->generatorPrototype();
    installGeneratorMethod<Generator, Generator::ResumeMode::Next>(globalObject, prototype, names.next);
    installGeneratorMethod<Generator, Generator::ResumeMode::Return>(globalObject, prototype, names.returnKeyword);
    installGeneratorMethod<Generator, Generator::ResumeMode::Throw>(globalObject, prototype, names.throwKeyword);
    prototype = globalObject->asyncGeneratorPrototype();
    installGeneratorMethod<AsyncGenerator, Generator::ResumeMode::Next>(globalObject, prototype, names.next);
    installGeneratorMethod<AsyncGenerator, Generator::ResumeMode::Return>(globalObject, prototype, names.returnKeyword);
    installGeneratorMethod<AsyncGenerator, Generator::ResumeMode::Throw>(globalObject, prototype, names.throwKeyword);
}

uint32_t State::resumeFunctionOf(uint32_t generatorFunction) const
{
    std::span functions { data.at<std::array<uint32_t, 2>>(data.generatorFunctionsOffset), data.numberOfGeneratorFunctions };
    auto found = std::ranges::lower_bound(functions, generatorFunction, { }, [](auto& generatorAndResumeFunction) {
        return generatorAndResumeFunction[0];
    });
    RELEASE_ASSERT(found != functions.end() && (*found)[0] == generatorFunction);
    return (*found)[1];
}

void State::setPrototypeOfGenerators(VM& vm, uint32_t resumeFunction, bool isAsync, JSObject* prototype)
{
    JSGlobalObject* globalObject = instance.globalObject;
    Structure* structure = isAsync ? AsyncGenerator::createStructure(vm, globalObject, prototype) : Generator::createStructure(vm, globalObject, prototype);
    m_generatorStructures[resumeFunction] = structure;
    didCreate(structure);
}

Structure* State::generatorStructure(VM& vm, uint32_t resumeFunction, bool isAsync)
{
    if (!m_generatorStructures[resumeFunction]) {
        JSGlobalObject* globalObject = instance.globalObject;
        setPrototypeOfGenerators(vm, resumeFunction, isAsync, constructEmptyObject(globalObject, isAsync ? static_cast<JSObject*>(globalObject->asyncGeneratorPrototype()) : globalObject->generatorPrototype()));
    }
    return m_generatorStructures[resumeFunction];
}

static JSC_DECLARE_HOST_FUNCTION(getLiveBinding);

JSC_DEFINE_HOST_FUNCTION(getLiveBinding, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto name = callFrame->argument(0).toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(callFrame->thisValue().get(globalObject, name)));
}

JSFunction* State::liveBindingGetter(VM& vm)
{
    if (!m_liveBindingGetter)
        m_liveBindingGetter = JSFunction::create(vm, instance.globalObject, 1, String(), getLiveBinding, ImplementationVisibility::Private);
    return m_liveBindingGetter;
}

void resumeAsyncFunction(JSGlobalObject* globalObject, VM& vm, JSValue context, JSValue resolution, bool isRejected)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (auto* generator = dynamicDowncast<AsyncGenerator>(context)) {
        generator->didAwaitReturn(globalObject, resolution, isRejected);
        return;
    }
    Closure* closure = uncheckedDowncast<Closure>(context);
    Object* frame = uncheckedDowncast<Object>(closure->environment());
    const FieldData* fields = frame->layout().fields();
    if (auto* generator = dynamicDowncast<AsyncGenerator>(frame->read(fields[CoroutineFrame::promiseField]))) {
        generator->resumeFromAwait(globalObject, resolution, isRejected);
        return;
    }
    storeResolution(globalObject, frame, resolution, isRejected);
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(static_cast<int32_t>(isRejected)));
    call(globalObject, closure, Closure::getCallData(closure), jsUndefined(), arguments);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (!scope.clearExceptionExceptTermination())
            return;
        uncheckedDowncast<JSPromise>(frame->read(fields[CoroutineFrame::promiseField]))->reject(vm, error);
    }
}

Format::FunctionKind Closure::kind() const
{
    return structure()->aotInstance()->typed->kindOf(m_function);
}

bool Closure::isDeclaredAs(uint8_t asyncOrGenerator) const
{
    return structure()->aotInstance()->typed->kindAndDeclarationOf(m_function) & asyncOrGenerator;
}

bool Closure::sharesPrototypeProperty() const
{
    return isDeclaredAs(functionIsGenerator) && structure()->aotInstance()->typedClosures[m_function] != this;
}

static CallData nativeCallData(NativeFunction function)
{
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = function;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

const LayoutData* Closure::classData() const
{
    return structure()->aotInstance()->typed->classConstructedBy(m_function);
}

CallData Closure::getCallData(JSCell* cell)
{
    if (uncheckedDowncast<Closure>(cell)->kind() == Format::FunctionKind::Constructor)
        return nativeCallData(callClassConstructor);
    return nativeCallData(callClosure);
}

JSValue Closure::callAsConstructor(JSGlobalObject* globalObject, JSValue thisValue, JSValue newTarget, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer all;
    appendArgumentsOfConstruct(all, this, newTarget, arguments);
    if (all.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    // Not what getCallData() says: [[Call]] of a class throws.
    RELEASE_AND_RETURN(scope, call(globalObject, this, nativeCallData(callClosure), thisValue, all));
}

CallData Closure::getConstructData(JSCell* cell)
{
    Closure* closure = uncheckedDowncast<Closure>(cell);
    switch (closure->kind()) {
    case Format::FunctionKind::Plain:
        if (closure->isDeclaredAs(functionIsAsync | functionIsGenerator))
            return { };
        [[fallthrough]];
    case Format::FunctionKind::Constructor:
        return nativeCallData(constructClosure);
    default:
        return { };
    }
}

// It is made when it is first asked for. False: a function of its kind has none.
bool Closure::makePrototypeProperty(VM& vm, State& state)
{
    JSObject* prototype;
    if (isDeclaredAs(functionIsGenerator)) {
        // What the generators it makes inherit from.
        prototype = state.generatorStructure(vm, state.resumeFunctionOf(m_function), isDeclaredAs(functionIsAsync))->storedPrototypeObject();
    } else if (kind() == Format::FunctionKind::Plain && !isDeclaredAs(functionIsAsync)) {
        // What `new` on an ordinary function gives its objects.
        prototype = constructEmptyObject(state.instance.globalObject);
        prototype->putDirect(vm, vm.propertyNames->constructor, this, static_cast<unsigned>(PropertyAttribute::DontEnum));
    } else
        return false;
    putDirect(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    return true;
}

bool Closure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    if (Base::getOwnPropertySlot(object, globalObject, propertyName, slot))
        return true;
    State& state = *object->structure()->aotInstance()->typed;
    if (const LayoutData* ofClass = closure->classData()) {
        // Until its initializer has run there is no such property.
        const FieldData* field = state.staticField(*ofClass, propertyName.uid());
        if (JSValue value = field ? state.readVariable(closure, *ofClass, *field) : JSValue()) {
            slot.setValue(object, static_cast<unsigned>(PropertyAttribute::None), value);
            return true;
        }
    }
    if (closure->m_hasReifiedProperties)
        return false;
    if (propertyName == vm.propertyNames->length) {
        slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, jsNumber(state.data.at<uint16_t>(state.data.functionLengthsOffset)[closure->m_function]));
        return true;
    }
    if (propertyName == vm.propertyNames->prototype && closure->makePrototypeProperty(vm, state))
        return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
    if (propertyName == vm.propertyNames->name) {
        slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, state.string(vm, state.data.at<uint32_t>(state.data.functionsOffset)[closure->m_function]));
        return true;
    }
    return false;
}

void Closure::reifyProperties(VM& vm, JSGlobalObject* globalObject)
{
    if (m_hasReifiedProperties)
        return;
    const LayoutData* ofClass = classData();
    for (const Identifier* name : { &vm.propertyNames->length, &vm.propertyNames->name, &vm.propertyNames->prototype }) {
        // `static name = ..` stays in its variable.
        if (ofClass && structure()->aotInstance()->typed->staticField(*ofClass, name->impl()))
            continue;
        PropertySlot slot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);
        if (!getDirect(vm, *name) && getOwnPropertySlot(this, globalObject, *name, slot) && !getDirect(vm, *name))
            putDirect(vm, *name, slot.getValue(globalObject, *name), slot.attributes());
    }
    m_hasReifiedProperties = true;
}

static bool isLazyPropertyOfClosure(VM& vm, PropertyName name)
{
    return name == vm.propertyNames->length || name == vm.propertyNames->name || name == vm.propertyNames->prototype;
}

static String describeStaticField(State& state, const LayoutData& ofClass, PropertyName name)
{
    return makeString("static field '"_s, StringView(name.uid()), "' of class "_s, state.nameOf(ofClass));
}

// False: it has thrown.
static bool assignStaticField(JSGlobalObject* globalObject, State& state, Closure* classObject, const LayoutData& ofClass, const FieldData& field, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String failure;
    JSValue held = state.holdToTypeOf(globalObject, field, value, failure);
    RETURN_IF_EXCEPTION(scope, false);
    if (!held || !state.writeVariable(vm, classObject, ofClass, field, held)) {
        throwTypeError(globalObject, scope, state.describeRefusedStore(describeStaticField(state, ofClass, name), field, value, failure));
        return false;
    }
    return true;
}

// Compiled code that calls a generator function has no closure at hand to ask for its `prototype`. So the closures of a function share
// theirs, and only where there is one closure can it be replaced.
static void throwSharesPrototypeProperty(JSGlobalObject* globalObject, ThrowScope& scope, State& state, Closure* closure)
{
    throwTypeError(globalObject, scope, makeString("Cannot replace the 'prototype' of generator function '"_s, state.nameOfFunction(closure->functionIndex()), "': there may be several closures of the function, and they share it"_s));
}

bool Closure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    State& state = *cell->structure()->aotInstance()->typed;
    const LayoutData* ofClass = closure->classData();
    if (const FieldData* field = ofClass ? state.staticField(*ofClass, name.uid()) : nullptr) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (slot.thisValue() != JSValue(closure))
            RELEASE_AND_RETURN(scope, setStepByStep(globalObject, closure, name, value, slot));
        RELEASE_AND_RETURN(scope, assignStaticField(globalObject, state, closure, *ofClass, *field, name, value));
    }
    if (name == vm.propertyNames->prototype && slot.thisValue() == JSValue(closure) && closure->sharesPrototypeProperty()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwSharesPrototypeProperty(globalObject, scope, state, closure);
        return false;
    }
    if (isLazyPropertyOfClosure(vm, name))
        closure->reifyProperties(vm, globalObject);
    bool result = Base::put(cell, globalObject, name, value, slot);
    if (name == vm.propertyNames->prototype) [[unlikely]] {
        // A cache would store to the next closure without coming here.
        slot.disableCaching();
        state.didChangePrototypeOf(vm, closure);
    }
    return result;
}

bool Closure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* closure = uncheckedDowncast<Closure>(object);
    State& state = *object->structure()->aotInstance()->typed;
    const LayoutData* ofClass = closure->classData();
    if (const FieldData* field = ofClass ? state.staticField(*ofClass, name.uid()) : nullptr) [[unlikely]] {
        // ValidateAndApplyPropertyDescriptor. What the variable holds is a writable, enumerable and configurable data property.
        JSValue current = state.readVariable(closure, *ofClass, *field);
        PropertyDescriptor currentDescriptor;
        if (current)
            currentDescriptor.setDescriptor(current, static_cast<unsigned>(PropertyAttribute::None));
        bool isValid = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, closure->isStructureExtensible(), descriptor, !!current, currentDescriptor, shouldThrow);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isValid)
            return false;
        if (current ? descriptor.attributesOverridingCurrent(currentDescriptor) : descriptor.attributes()) {
            throwTypeError(globalObject, scope, makeString("Cannot redefine "_s, describeStaticField(state, *ofClass, name), ": it stays a writable, enumerable and configurable data property"_s));
            return false;
        }
        // What is there stays as it is.
        if (current && !descriptor.value())
            return true;
        RELEASE_AND_RETURN(scope, assignStaticField(globalObject, state, closure, *ofClass, *field, name, descriptor.value() ? descriptor.value() : jsUndefined()));
    }
    if (name == vm.propertyNames->prototype && descriptor.value() && closure->sharesPrototypeProperty()) [[unlikely]] {
        throwSharesPrototypeProperty(globalObject, scope, state, closure);
        return false;
    }
    if (ofClass && ofClass->hasFixedMembers && !closure->getDirect(vm, name) && state.inheritsFixedStaticMethod(*ofClass, name.uid())) [[unlikely]] {
        throwTypeError(globalObject, scope, makeString("Cannot define property '"_s, StringView(name.uid()), "' on class "_s, state.nameOf(*ofClass), ": it is a static method or accessor of a class it extends"_s));
        return false;
    }
    if (isLazyPropertyOfClosure(vm, name))
        closure->reifyProperties(vm, globalObject);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (name == vm.propertyNames->prototype) [[unlikely]]
        state.didChangePrototypeOf(vm, closure);
    return result;
}

bool Closure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    State& state = *cell->structure()->aotInstance()->typed;
    const LayoutData* ofClass = closure->classData();
    if (const FieldData* field = ofClass ? state.staticField(*ofClass, name.uid()) : nullptr) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!state.readVariable(closure, *ofClass, *field))
            return true;
        throwTypeError(globalObject, scope, makeString("Cannot delete "_s, describeStaticField(state, *ofClass, name)));
        return false;
    }
    if (isLazyPropertyOfClosure(vm, name))
        closure->reifyProperties(vm, globalObject);
    return Base::deleteProperty(cell, globalObject, name, slot);
}

bool Closure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const LayoutData* ofClass = uncheckedDowncast<Closure>(object)->classData();
    if (!ofClass || !ofClass->hasFixedMembers || object->getPrototypeDirect() == prototype)
        RELEASE_AND_RETURN(scope, Base::setPrototype(object, globalObject, prototype, shouldThrow));
    return typeError(globalObject, scope, shouldThrow, makeString("Cannot change the prototype of class "_s, object->structure()->aotInstance()->typed->nameOf(*ofClass)));
}

// `length`, `name` and `prototype` are made when they are asked for. The static fields that are in variables come next.
void Closure::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    if (mode == DontEnumPropertiesMode::Include) {
        for (const Identifier* name : { &vm.propertyNames->length, &vm.propertyNames->name, &vm.propertyNames->prototype }) {
            PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
            if (!object->getDirect(vm, *name) && getOwnPropertySlot(object, globalObject, *name, slot))
                names.add(*name);
        }
    }
    Closure* closure = uncheckedDowncast<Closure>(object);
    const LayoutData* ofClass = closure->classData();
    if (!ofClass)
        return;
    State& state = *object->structure()->aotInstance()->typed;
    for (auto& field : ofClass->staticFields()) {
        if (state.readVariable(closure, *ofClass, field))
            names.add(state.name(vm, field.name));
    }
}

static size_t sizeOfZeroedTable(size_t count, size_t size)
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(count, 1) * size);
}

template<typename T>
static T* createZeroedTable(size_t count)
{
    return static_cast<T*>(OSAllocator::reserveAndCommit(sizeOfZeroedTable(count, sizeof(T))));
}

template<typename T>
static void destroyZeroedTable(T* table, size_t count)
{
    OSAllocator::decommitAndRelease(table, sizeOfZeroedTable(count, sizeof(T)));
}

State::State(VM& vm, Instance& instance, const Data& data)
    : instance(instance)
    , data(data)
    , extraProperties(vm)
    , m_structuresWithOtherPrototype(vm)
{
    names = createZeroedTable<Identifier>(data.numberOfStrings);
    structures = createZeroedTable<Structure*>(data.numberOfLayouts);
    prototypesOfClasses = createZeroedTable<JSObject*>(data.numberOfLayoutIndices);
    m_fieldTables = createZeroedTable<FieldTable*>(data.numberOfLayouts);
    m_orderTables = createZeroedTable<OrderTable*>(data.numberOfLayouts);
    m_sizesWithRoomToSpecialize = createZeroedTable<uint32_t>(data.numberOfLayouts);
    m_generatorStructures = createZeroedTable<Structure*>(data.numberOfFunctions);
    instance.typedStatics = createZeroedTable<uint8_t>(data.sizeOfStatics);
    instance.typedAllocations = createZeroedTable<Instance::TypedAllocation>(data.numberOfAllocations);
    instance.typedShapeSets = data.at<uint32_t>(data.shapeSetsOffset);
    instance.typedStrings = createZeroedTable<JSCell*>(data.numberOfStrings);
    instance.typedFunctions = createZeroedTable<uint64_t>(data.numberOfFunctions);
    instance.typedClosures = createZeroedTable<JSCell*>(data.numberOfFunctions);
    instance.typedBuiltins = createZeroedTable<JSCell*>(Builtins::singleton().numberOfCells());
    installGeneratorMethods(instance.globalObject);
}

State::~State()
{
    for (uint32_t index : m_createdNames)
        names[index] = Identifier();
    destroyZeroedTable(names, data.numberOfStrings);
    destroyZeroedTable(structures, data.numberOfLayouts);
    destroyZeroedTable(prototypesOfClasses, data.numberOfLayoutIndices);
    destroyZeroedTable(m_fieldTables, data.numberOfLayouts);
    destroyZeroedTable(m_orderTables, data.numberOfLayouts);
    destroyZeroedTable(m_sizesWithRoomToSpecialize, data.numberOfLayouts);
    destroyZeroedTable(m_generatorStructures, data.numberOfFunctions);
    destroyZeroedTable(instance.typedStatics, data.sizeOfStatics);
    destroyZeroedTable(instance.typedAllocations, data.numberOfAllocations);
    destroyZeroedTable(instance.typedStrings, data.numberOfStrings);
    destroyZeroedTable(instance.typedFunctions, data.numberOfFunctions);
    destroyZeroedTable(instance.typedClosures, data.numberOfFunctions);
    destroyZeroedTable(instance.typedBuiltins, Builtins::singleton().numberOfCells());
}

void State::didCreate(JSCell* cell)
{
    Locker locker { m_cellsLock };
    m_cells.append(cell);
}

// UTF-8, but that half of a surrogate pair on its own, which a string of the language may have, is encoded like any other code unit.
static String textOf(std::span<const uint8_t> bytes)
{
    if (charactersAreAllASCII(byteCast<Latin1Character>(bytes)))
        return StringImpl::createWithoutCopying(byteCast<Latin1Character>(bytes));
    if (String text = String::fromUTF8(bytes); !text.isNull())
        return text;
    Vector<char16_t> units;
    for (size_t i = 0; i < bytes.size();) {
        uint8_t lead = bytes[i];
        unsigned length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        RELEASE_ASSERT(i + length <= bytes.size());
        char32_t character = length == 1 ? lead : lead & (0x7f >> length);
        for (unsigned k = 1; k < length; ++k)
            character = character << 6 | (bytes[i + k] & 0x3f);
        i += length;
        if (U_IS_BMP(character))
            units.append(static_cast<char16_t>(character));
        else {
            units.append(U16_LEAD(character));
            units.append(U16_TRAIL(character));
        }
    }
    return String(units.span());
}

const Identifier& State::name(VM& vm, uint32_t index)
{
    Identifier& name = names[index];
    if (name.isNull()) {
        auto bytes = data.string(index);
        if (isPrivateName(bytes)) {
            // A name of its own, that no key the program comes by is equal to.
            auto written = bytes.subspan(1, std::ranges::find(bytes, static_cast<uint8_t>('@')) - bytes.begin() - 1);
            name = Identifier::fromUid(PrivateSymbolImpl::create(*textOf(written).impl()).get());
        } else
            name = Identifier::fromString(vm, textOf(bytes));
        m_createdNames.append(index);
    }
    return name;
}

JSString* State::string(VM& vm, uint32_t index)
{
    JSCell*& cell = instance.typedStrings[index];
    if (!cell) {
        cell = jsString(vm, textOf(data.string(index)));
        didCreate(cell);
    }
    return uncheckedDowncast<JSString>(cell);
}

bool State::hasText(uint32_t string, StringView text) const
{
    auto characters = byteCast<Latin1Character>(data.string(string));
    if (characters.size() < text.length() || isPrivateName(data.string(string)))
        return false;
    bool isASCII = charactersAreAllASCII(characters);
    if (characters.size() == text.length())
        return isASCII && equal(text, characters);
    return !isASCII && text == textOf(data.string(string));
}

bool State::isName(uint32_t string, UniquedStringImpl* name) const
{
    if (const Identifier& known = names[string]; !known.isNull())
        return known.impl() == name;
    return hasText(string, StringView { *name });
}

// `#name` of a class is no property. Nor is what has no name.
bool State::isPrivate(const FieldData& field) const
{
    auto bytes = data.string(field.name);
    return bytes.empty() || isPrivateName(bytes);
}

const FieldData* State::field(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
    FieldTable*& table = m_fieldTables[layout.shape];
    if (!table) [[unlikely]] {
        auto created = makeUnique<FieldTable>();
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            if (!data.string(layout.fields()[i].name).empty())
                created->add(this->name(vm, layout.fields()[i].name).impl(), i);
        }
        table = created.get();
        m_createdFieldTables.append(WTF::move(created));
    }
    auto found = table->find(name);
    return found == table->end() ? nullptr : layout.fields() + found->value;
}

bool State::mayBeAbsent(const FieldData& field) const
{
    return typeOf(field).primitives & Primitive::Absent;
}

Object* State::compiledObject(JSValue value) const
{
    auto* object = dynamicDowncast<Object>(value);
    return object && isObjectLayout(object->layout().kind) && &object->state().data == &data ? object : nullptr;
}

bool State::passes(const RuntimeShapeTest& test, uint32_t shape) const
{
    uint32_t placeFromLast = test.first + test.count - 1 - shape;
    return placeFromLast < test.count && (test.set == UINT32_MAX || data.at<uint32_t>(data.shapeSetsOffset)[(test.set + placeFromLast) / 32] >> (test.set + placeFromLast) % 32 & 1);
}

bool State::admitsShape(const RuntimeType& type, uint32_t shape) const
{
    for (uint32_t i = 0; i < type.numberOfViews; ++i) {
        if (passes(type.views()[i], shape))
            return true;
    }
    return false;
}

bool State::isAmongLiterals(const FieldData& field, JSValue value) const
{
    bool isNumber = value.isNumber();
    if (!field.numberOfLiterals || (!isNumber && !value.isString()))
        return true;
    bool hasLiteralsOfTheKind = false;
    for (auto& literal : std::span { data.at<LiteralData>(field.literalsOffset), field.numberOfLiterals }) {
        if (!!literal.isNumber != isNumber)
            continue;
        hasLiteralsOfTheKind = true;
        if (isNumber ? literal.number == value.asNumber() : hasText(literal.string, asString(value)->tryGetValue()))
            return true;
    }
    return !hasLiteralsOfTheKind;
}

bool State::fits(const RuntimeType& type, JSValue value) const
{
    if (value.isUndefined())
        return type.primitives & Primitive::Undefined;
    if (value.isNull())
        return type.primitives & Primitive::Null;
    if (value.isBoolean())
        return type.primitives & (value.asBoolean() ? Primitive::True : Primitive::False);
    if (value.isNumber()) {
        double number = value.asNumber();
        bool isInteger = number == std::trunc(number) && std::abs(number) <= static_cast<double>(maxKeptInteger) && !(!number && std::signbit(number));
        if (isInteger)
            return type.primitives & Primitive::Int && number >= static_cast<double>(type.minInteger) && number <= static_cast<double>(type.maxInteger);
        return type.primitives & Primitive::Double;
    }
    if (value.isBigInt())
        return type.primitives & Primitive::BigInt;
    if (!value.isCell())
        return false;
    JSCell* cell = value.asCell();
    if (cell->isString())
        return type.primitives & Primitive::String;
    if (cell->isSymbol())
        return type.primitives & Primitive::Symbol;
    if (!cell->isObject())
        return false;
    return type.admitsForeignObjects || fitsAsOneOfItsOwn(type, asObject(cell));
}

bool State::fitsAsOneOfItsOwn(const RuntimeType& type, JSObject* cell) const
{
    if (type.allowsAnyObject)
        return true;
    if (Object* object = compiledObject(cell))
        return admitsShape(type, object->shape());
    for (uint32_t i = 0; i < type.numberOfJSTypes; ++i) {
        if (type.jsTypes()[i] == cell->type())
            return true;
    }
    return type.allowsAnyFunction && cell->isCallable();
}

String State::describe(JSValue value)
{
    if (!value)
        return "nothing"_s;
    if (value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean())
        return value.asBoolean() ? "true"_s : "false"_s;
    if (value.isNumber())
        return String::number(value.asNumber());
    if (value.isString()) {
        StringImpl* text = asString(value)->tryGetValueImpl();
        if (!text || text->length() > 40)
            return "a string"_s;
        return makeString('"', StringView { *text }, '"');
    }
    if (value.isSymbol())
        return "a symbol"_s;
    if (value.isBigInt())
        return "a bigint"_s;
    if (auto* object = dynamicDowncast<Object>(value))
        return makeString("an object of type "_s, object->state().nameOf(object->layout()));
    // See EngineClass::type.
    if (is<JSArray>(value))
        return isJSArray(value) ? "an array"_s : "an array that is not plain (of a class that extends Array, or changed since it was made)"_s;
    return value.isCallable() ? "a function"_s : "an object"_s;
}

static void appendAlternative(StringBuilder& result, bool has, StringView name)
{
    if (has)
        result.append(result.isEmpty() ? ""_s : " | "_s, name);
}

static void appendPrimitives(StringBuilder& result, uint32_t primitives)
{
    appendAlternative(result, (primitives & Primitive::Boolean) == Primitive::Boolean, "boolean"_s);
    appendAlternative(result, (primitives & Primitive::Boolean) == Primitive::True, "true"_s);
    appendAlternative(result, (primitives & Primitive::Boolean) == Primitive::False, "false"_s);
    appendAlternative(result, primitives & Primitive::Double, "number"_s);
    appendAlternative(result, (primitives & Primitive::Number) == Primitive::Int, "integer"_s);
    appendAlternative(result, primitives & Primitive::String, "string"_s);
    appendAlternative(result, primitives & Primitive::Symbol, "symbol"_s);
    appendAlternative(result, primitives & Primitive::BigInt, "bigint"_s);
    appendAlternative(result, primitives & Primitive::Undefined, "undefined"_s);
    appendAlternative(result, primitives & Primitive::Null, "null"_s);
    appendAlternative(result, result.isEmpty(), "never"_s);
}

String State::describe(const RuntimeType& type) const
{
    StringBuilder result;
    for (uint32_t i = 0; i < type.numberOfViews; ++i) {
        uint32_t shape = type.views()[i].shapeOfView;
        appendAlternative(result, true, shape ? nameOf(layoutOfShape(shape)) : "object"_str);
    }
    for (uint32_t i = 0; i < type.numberOfJSTypes; ++i) {
        ASCIILiteral name = "object"_s;
        for (auto& engineClass : Builtins::singleton().classes()) {
            if (static_cast<uint32_t>(engineClass.type) == type.jsTypes()[i]) {
                name = engineClass.text;
                break;
            }
        }
        appendAlternative(result, true, name);
    }
    appendAlternative(result, type.allowsAnyObject, "object"_s);
    appendAlternative(result, type.allowsAnyFunction && !type.allowsAnyObject, "function"_s);
    if ((type.primitives & Primitive::Number) == Primitive::Int && type.minInteger >= -maxExactInteger && type.maxInteger <= maxExactInteger) {
        appendAlternative(result, true, makeString("integer from "_s, type.minInteger, " to "_s, type.maxInteger));
        appendPrimitives(result, type.primitives & ~Primitive::Number);
    } else
        appendPrimitives(result, type.primitives);
    return result.toString();
}

String State::describeKinds(uint32_t kinds, uint32_t name) const
{
    StringBuilder result;
    appendAlternative(result, name != UINT32_MAX || kinds & typeAllowsObjects, name != UINT32_MAX ? textOf(data.string(name)) : "object"_str);
    appendAlternative(result, kinds & typeAllowsFunctions, "function"_s);
    appendPrimitives(result, kinds & Primitive::All);
    return result.toString();
}

String State::nameOf(const LayoutData& layout) const
{
    if (auto name = data.string(layout.name); !name.empty())
        return textOf(name);
    static constexpr uint32_t maxNamesShown = 4;
    StringBuilder result;
    result.append('{');
    for (uint32_t i = 0; i < std::min(layout.numberOfFields, maxNamesShown); ++i)
        result.append(i ? ", "_s : " "_s, textOf(data.string(layout.fields()[i].name)));
    result.append(layout.numberOfFields > maxNamesShown ? ", ... }"_s : " }"_s);
    return result.toString();
}

String State::describeRefusedStore(const String& place, const FieldData& field, JSValue value, const String& failure) const
{
    String why = field.flags & fieldIsImmutable ? "it is read-only"_str : makeString("it holds "_s, describe(ownTypeOf(field)));
    return makeString("Cannot store "_s, describe(value), " to "_s, place, ": "_s, why, failure.isEmpty() ? ""_s : " ("_s, failure, failure.isEmpty() ? ""_s : ")"_s);
}

String State::nameOfFunction(uint32_t function) const
{
    return textOf(data.string(data.at<uint32_t>(data.functionsOffset)[function]));
}

String State::describePosition(uint32_t function, uint32_t position) const
{
    String name = function < data.numberOfFunctions ? nameOfFunction(function) : String();
    return makeString(" (in "_s, name.isEmpty() ? "an anonymous function"_str : name, ", at offset "_s, position, ')');
}

const LayoutData* State::classWithIndex(uint32_t index) const
{
    uint32_t offset = data.at<uint32_t>(data.classesOffset)[index];
    return offset ? data.at<LayoutData>(offset) : nullptr;
}

const LayoutData* State::classConstructedBy(uint32_t function) const
{
    if (kindOf(function) != Format::FunctionKind::Constructor)
        return nullptr;
    std::span constructors { data.at<std::array<uint32_t, 2>>(data.constructorsOffset), data.numberOfConstructors };
    auto found = std::ranges::lower_bound(constructors, function, { }, [](auto& constructorAndClass) {
        return constructorAndClass[0];
    });
    RELEASE_ASSERT(found != constructors.end() && (*found)[0] == function);
    return classWithIndex((*found)[1]);
}

static uint8_t* addressOfVariable(const Instance& instance, const Closure* classObject, const LayoutData& ofClass, const FieldData& variable)
{
    if (ofClass.environment == LayoutData::noBase)
        return instance.typedStatics + variable.offset;
    return reinterpret_cast<uint8_t*>(classObject->environment()) + variable.offset;
}

JSValue State::readVariable(const Closure* classObject, const LayoutData& ofClass, const FieldData& variable) const
{
    return readAs(variable.store, addressOfVariable(instance, classObject, ofClass, variable));
}

bool State::writeVariable(VM& vm, Closure* classObject, const LayoutData& ofClass, const FieldData& variable, JSValue value)
{
    if (variable.flags & fieldIsImmutable || !fits(typeOf(variable), value) || !storeAs(variable.store, addressOfVariable(instance, classObject, ofClass, variable), value))
        return false;
    // The module variables are roots.
    if (ofClass.environment != LayoutData::noBase && holdsCells(variable.store))
        vm.writeBarrier(classObject->environment(), value);
    return true;
}

const FieldData* State::staticField(const LayoutData& ofClass, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return nullptr;
    for (auto& field : ofClass.staticFields()) {
        if (isName(field.name, name))
            return &field;
    }
    return nullptr;
}

// The first class that declares the name decides: that is where a search by name ends.
bool State::isFixedIn(std::span<const MethodData> methods, UniquedStringImpl* name, bool& isDeclared) const
{
    for (auto& method : methods) {
        if (method.flags & methodHasComputedName || !isName(method.name, name))
            continue;
        isDeclared = true;
        if (method.flags & methodMayBeReplaced)
            return false;
    }
    return isDeclared;
}

bool State::inheritsFixedStaticMethod(const LayoutData& ofClass, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return false;
    bool isDeclared = false;
    for (const LayoutData* next = baseOf(ofClass); next && !isDeclared; next = baseOf(*next)) {
        if (isFixedIn(next->staticMethods(), name, isDeclared))
            return true;
    }
    return false;
}

const EngineClass* State::engineClassConstructedBy(JSObject* constructor)
{
    auto& builtins = Builtins::singleton();
    if (m_engineClassesByConstructor.isEmpty()) {
        for (auto& engineClass : builtins.classes()) {
            if (builtins.mayBeExtended(engineClass.name) && builtins.isGlobal(engineClass.name))
                m_engineClassesByConstructor.add(asObject(builtins.findCell(instance.globalObject, builtins.cellOf(engineClass.name))), engineClass.index);
        }
    }
    auto found = m_engineClassesByConstructor.find(constructor);
    return found == m_engineClassesByConstructor.end() ? nullptr : &builtins.classes()[found->value];
}

// SetFunctionName, for a name that is computed.
static void setNameOfMember(VM& vm, Closure* function, const Identifier& key, Format::MethodKind kind)
{
    String name = key.string();
    if (UniquedStringImpl* uid = key.impl(); uid->isSymbol())
        name = static_cast<SymbolImpl&>(*uid).isNullSymbol() ? emptyString() : makeString('[', StringView(uid), ']');
    if (kind != Format::MethodKind::Method)
        name = makeString(kind == Format::MethodKind::Getter ? "get "_s : "set "_s, name);
    function->putDirect(vm, vm.propertyNames->name, jsString(vm, name), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
}

// ClassDefinitionEvaluation, but for what can be seen to happen at the declaration, which compiled code does there: what stands after
// `extends` and the computed names are evaluated, the static fields are initialized, the static blocks run.
void State::makeClass(JSGlobalObject* globalObject, Closure* classObject, JSValue base)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const LayoutData& ofClass = *classObject->classData();
    RELEASE_ASSERT(!classObject->getDirect(vm, vm.propertyNames->prototype));
    if (ofClass.environment != LayoutData::noBase)
        RELEASE_ASSERT(classObject->environment() && uncheckedDowncast<Object>(classObject->environment())->layout().index == ofClass.environment);

    JSValue parentOfPrototype = globalObject->objectPrototype();
    JSObject* parentOfClassObject = nullptr;
    if (const LayoutData* baseClass = baseOf(ofClass)) {
        // Of a class that is evaluated once there is one class object. Of any other, compiled code says which this one extends.
        auto* baseClassObject = baseClass->isEvaluatedOnce ? closure(vm, baseClass->classConstructor) : dynamicDowncast<Closure>(base);
        RELEASE_ASSERT(baseClassObject && baseClassObject->classData() == baseClass);
        parentOfClassObject = baseClassObject;
        parentOfPrototype = baseClassObject->getDirect(vm, vm.propertyNames->prototype);
        RELEASE_ASSERT(parentOfPrototype);
    } else if (ofClass.hasForeignRoot && base.isNull())
        parentOfPrototype = jsNull();
    else if (ofClass.hasForeignRoot) {
        if (!base.isConstructor()) {
            throwTypeError(globalObject, scope, "The superclass is not a constructor."_s);
            return;
        }
        parentOfClassObject = asObject(base);
        parentOfPrototype = parentOfClassObject->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, void());
        if (!parentOfPrototype.isObject() && !parentOfPrototype.isNull()) {
            throwTypeError(globalObject, scope, "The value of the superclass's prototype property is not an object or null."_s);
            return;
        }
        // Compiled code calls the methods of a class of the engine without looking them up, unless the class is seen to be extended.
        for (JSObject* next = parentOfClassObject; next;) {
            if (auto* engineClass = engineClassConstructedBy(next); engineClass && !data.at<uint8_t>(data.extendedClassesOffset)[engineClass->index]) {
                throwTypeError(globalObject, scope, makeString("Class "_s, nameOf(ofClass), " extends "_s, engineClass->text, ", which the compiler has not seen any class extend: what stands after 'extends' has to have the type of that class"_s));
                return;
            }
            if (auto* proxy = dynamicDowncast<ProxyObject>(next))
                next = proxy->target();
            else if (auto* bound = dynamicDowncast<JSBoundFunction>(next))
                next = bound->targetFunction();
            else
                next = next->getPrototypeDirect().getObject();
        }
    }

    // See below. The Structure such a prototype object ends up with is marked, so it starts from one that nothing else has.
    bool keepsMethodsReplaceable = ofClass.isLaidOut && std::ranges::any_of(ofClass.methods(), [](auto& method) {
        return method.flags & methodMayBeReplaced;
    });
    JSObject* prototype;
    if (keepsMethodsReplaceable)
        prototype = constructEmptyObject(vm, JSFinalObject::createStructure(vm, globalObject, parentOfPrototype, JSFinalObject::defaultInlineCapacity));
    else
        prototype = parentOfPrototype.isNull() ? constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure()) : constructEmptyObject(globalObject, asObject(parentOfPrototype));
    if (parentOfClassObject)
        classObject->setPrototypeDirect(vm, parentOfClassObject);
    classObject->putDirect(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    prototype->putDirect(vm, vm.propertyNames->constructor, classObject, static_cast<unsigned>(PropertyAttribute::DontEnum));
    if (ofClass.isEvaluatedOnce) {
        prototypesOfClasses[ofClass.index] = prototype;
        didCreate(prototype);
    }

    auto isPrivate = [&](const MethodData& method) {
        return !(method.flags & methodHasComputedName) && isPrivateName(data.string(method.name));
    };
    auto keyFor = [&](const MethodData& method) {
        if (!(method.flags & methodHasComputedName))
            return name(vm, method.name);
        RELEASE_ASSERT(method.name < ofClass.numberOfVariables);
        // The declaration has stored it.
        JSValue key = readVariable(classObject, ofClass, ofClass.variables()[method.name]);
        RELEASE_ASSERT(key && (key.isString() || key.isSymbol()));
        return key.toPropertyKey(globalObject);
    };
    // Nothing is in the way: the objects are new, and this goes past what a class object refuses.
    auto define = [&](JSObject* target, const Identifier& key, const PropertyDescriptor& descriptor) {
        JSObject::defineOwnProperty(target, globalObject, key, descriptor, true);
        scope.assertNoException();
    };
    // `#name() {}` is no property. See findPrivateElement() for a class that is laid out. For any other, and for `static #name() {}`,
    // compiled code sees to it.
    auto defineAll = [&](JSObject* target, std::span<const MethodData> methods) {
        for (auto& method : methods) {
            bool isKeptForObjects = isPrivate(method) && target == prototype && ofClass.isLaidOut && !ofClass.isEvaluatedOnce;
            if (isPrivate(method) && !isKeptForObjects)
                continue;
            Identifier key = keyFor(method);
            auto kind = static_cast<Format::MethodKind>(method.kind);
            // Each evaluation of a class makes its own functions.
            Closure* function = ofClass.isEvaluatedOnce ? closure(vm, method.function) : Closure::create(vm, closureStructure(vm, method.function), entry(method.function), method.flags & methodTakesEnvironment ? classObject->environment() : nullptr, method.function);
            if (method.flags & methodHasComputedName)
                setNameOfMember(vm, function, key, kind);
            PropertyDescriptor descriptor;
            switch (kind) {
            case Format::MethodKind::Method:
                descriptor.setValue(function);
                descriptor.setWritable(!isKeptForObjects);
                break;
            case Format::MethodKind::Getter:
                descriptor.setGetter(function);
                break;
            case Format::MethodKind::Setter:
                descriptor.setSetter(function);
                break;
            }
            descriptor.setEnumerable(false);
            descriptor.setConfigurable(true);
            define(target, key, descriptor);
        }
    };
    defineAll(prototype, ofClass.methods());
    defineAll(classObject, ofClass.staticMethods());

    // From here on it cannot be stored to, redefined or deleted.
    auto fix = [&](JSObject* target, const Identifier& key, bool isAccessor) {
        PropertyDescriptor descriptor;
        descriptor.setConfigurable(false);
        if (!isAccessor)
            descriptor.setWritable(false);
        define(target, key, descriptor);
    };
    auto fixAll = [&](JSObject* target, std::span<const MethodData> methods) {
        for (auto& method : methods) {
            if (!isPrivate(method) && !(method.flags & methodMayBeReplaced))
                fix(target, keyFor(method), static_cast<Format::MethodKind>(method.kind) != Format::MethodKind::Method);
        }
    };
    if (ofClass.hasFixedMembers)
        fixAll(classObject, ofClass.staticMethods());
    if (!ofClass.isLaidOut)
        return;
    // Compiled code calls the methods by the shape of the object, and takes an absent field for `undefined` without looking here.
    if (!keepsMethodsReplaceable) {
        bool isImmutable = prototype->makePropertiesImmutable(vm);
        RELEASE_ASSERT(isImmutable);
        return;
    }
    // The methods the program is seen to replace stay as the language has them, writable and configurable: compiled code looks them up.
    // makePropertiesImmutable() knows no exceptions, so what it does to an object is done here to all the rest: every other property is
    // fixed, nothing can be added, the prototype stays, and what inherits a property that has become read-only may still have its own.
    fixAll(prototype, ofClass.methods());
    fix(prototype, vm.propertyNames->constructor, false);
    JSObject::preventExtensions(prototype, globalObject);
    scope.assertNoException();
    prototype->structure()->setInheritorsMayOverrideReadOnlyProperties(true);
}

JSObject* State::prototypeOfClass(VM& vm, uint32_t classIndex)
{
    JSObject*& prototype = prototypesOfClasses[classIndex];
    if (!prototype)
        closure(vm, classWithIndex(classIndex)->classConstructor);
    RELEASE_ASSERT(prototype);
    return prototype;
}

Object* State::createObjectOfClass(VM& vm, const LayoutData& ofClass, JSObject* prototype)
{
    RELEASE_ASSERT(ofClass.isLaidOut && ofClass.shape);
    Vector<uint16_t, 16> fields(ofClass.numberOfFields, [](size_t index) {
        return static_cast<uint16_t>(index);
    });
    return Object::createWithAllFields(vm, structureFor(vm, ofClass.shape, prototype), sizeWithRoomToSpecialize(ofClass.shape), orderStartingWith(ofClass, fields.span()));
}

// OrdinaryCreateFromConstructor. Whoever constructs makes the object, of the class that is constructed: the constructors of the classes
// that one extends work on it.
JSValue State::createThis(JSGlobalObject* globalObject, Closure* constructor, JSObject* newTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const LayoutData* ofClass = constructor->classData();
    if (ofClass && ofClass->hasForeignRoot)
        return jsUndefined();
    const LayoutData* made = ofClass;
    JSValue prototype;
    if (auto* target = dynamicDowncast<Closure>(newTarget); ofClass && target && target->structure()->aotInstance() == &instance) {
        const LayoutData* ofTarget = target->classData();
        for (const LayoutData* next = ofTarget; next; next = baseOf(*next)) {
            if (next != ofClass)
                continue;
            made = ofTarget;
            // It cannot be stored to or redefined.
            prototype = target->getDirect(vm, vm.propertyNames->prototype);
            break;
        }
    }
    if (!prototype) {
        // Compiled code calls the methods of the class by the shape of the object, whatever its prototype object has.
        if (ofClass && ofClass->isLaidOut && !ofClass->mayHaveOtherPrototype) {
            throwTypeError(globalObject, scope, makeString("Cannot construct an object of class "_s, nameOf(*ofClass), " for another constructor: the program is not seen to extend the class with one that is not compiled with types"_s));
            return { };
        }
        prototype = newTarget->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSObject* parent = prototype.isObject() ? asObject(prototype) : globalObject->objectPrototype();
    if (!ofClass) {
        // `function Point(this: Point, ..)`: as `new` makes it where it knows the function.
        if (const AllocationData* allocation = allocationOfThis(constructor->functionIndex()))
            return Object::create(vm, structureFor(vm, allocation->shape, parent), allocation->size, allocation->orderWord);
    }
    if (!ofClass || !ofClass->isLaidOut)
        return constructEmptyObject(globalObject, parent);
    if (!made->shape) {
        throwTypeError(globalObject, scope, makeString("Cannot construct an object of class "_s, nameOf(*made), ": the program is not seen to make any"_s));
        return { };
    }
    return createObjectOfClass(vm, *made, parent);
}

const AllocationData* State::allocationOfThis(uint32_t function) const
{
    std::span functions { data.at<std::array<uint32_t, 2>>(data.constructorFunctionsOffset), data.numberOfConstructorFunctions };
    auto found = std::ranges::lower_bound(functions, function, { }, [](auto& functionAndAllocation) {
        return functionAndAllocation[0];
    });
    return found != functions.end() && (*found)[0] == function ? &allocation((*found)[1]) : nullptr;
}

std::span<Structure* const> State::closureStructures(VM& vm)
{
    // All at once: compiled code takes it that they are there once a closure has been allocated.
    if (!m_closureStructures[0]) {
        JSGlobalObject* globalObject = instance.globalObject;
        JSObject* prototypes[] = { globalObject->functionPrototype(), globalObject->asyncFunctionPrototype(), globalObject->generatorFunctionPrototype(), globalObject->asyncGeneratorFunctionPrototype() };
        static_assert(std::size(prototypes) == Instance::numberOfTypedClosureStructures);
        for (unsigned i = 0; i < std::size(prototypes); ++i) {
            m_closureStructures[i] = Closure::createStructure(vm, globalObject, prototypes[i]);
            m_closureStructures[i]->setAOTInstance(&instance);
            instance.typedClosureStructureIDs[i] = m_closureStructures[i]->id().bits();
        }
    }
    return m_closureStructures;
}

Structure* State::closureStructure(VM& vm, uint32_t function)
{
    return closureStructures(vm)[closureStructureOf(kindAndDeclarationOf(function))];
}

// OrdinaryCreateFromConstructor: the `prototype` of the function if that is an object. It is a data property that cannot be deleted.
JSObject* State::prototypeForObjectsOf(VM& vm, Closure* constructor)
{
    constructor->reifyProperties(vm, instance.globalObject);
    JSValue prototype = constructor->getDirect(vm, vm.propertyNames->prototype);
    return prototype.isObject() ? asObject(prototype) : instance.globalObject->objectPrototype();
}

// The function has another `prototype`: what it makes from now on has that.
void State::didChangePrototypeOf(VM& vm, Closure* constructor)
{
    if (instance.typedClosures[constructor->functionIndex()] != constructor)
        return;
    if (constructor->isDeclaredAs(functionIsGenerator)) {
        // OrdinaryCreateFromConstructor, with what the realm has for a `prototype` that is no object.
        bool isAsync = constructor->isDeclaredAs(functionIsAsync);
        JSObject* prototype = constructor->getDirect(vm, vm.propertyNames->prototype).getObject();
        if (!prototype)
            prototype = isAsync ? static_cast<JSObject*>(instance.globalObject->asyncGeneratorPrototype()) : instance.globalObject->generatorPrototype();
        setPrototypeOfGenerators(vm, resumeFunctionOf(constructor->functionIndex()), isAsync, prototype);
        return;
    }
    for (uint32_t i = 0; i < data.numberOfAllocations; ++i) {
        if (allocation(i).constructedBy == constructor->functionIndex() && instance.typedAllocations[i].header)
            fill(vm, i);
    }
}

Structure* State::structure(VM& vm, uint32_t shape)
{
    Structure*& structure = structures[shape];
    if (structure)
        return structure;
    const LayoutData& layout = layoutOfShape(shape);
    JSValue prototype = instance.globalObject->objectPrototype();
    switch (layout.kind) {
    case LayoutKind::Closure:
        // Compiled code allocates every closure with it, and then sees to the others: see Instance::typedClosureStructureIDs.
        structure = closureStructures(vm)[0];
        return structure;
    case LayoutKind::Environment:
        prototype = jsNull();
        break;
    case LayoutKind::Record:
        break;
    case LayoutKind::Instance:
        // Where there is a class object for each evaluation, whoever makes an object says which it is of: see createObjectOfClass().
        if (layout.isEvaluatedOnce)
            prototype = prototypeOfClass(vm, layout.index);
        break;
    }
    structure = Object::createStructure(vm, instance, prototype, &layout);
    didCreate(structure);
    return structure;
}

Structure* State::structureFor(VM& vm, uint32_t shape, JSValue prototype)
{
    Structure* usual = structure(vm, shape);
    if (usual->storedPrototype() == prototype)
        return usual;
    std::pair<uint32_t, JSObject*> key { shape, prototype.getObject() };
    if (Structure* existing = m_structuresWithOtherPrototype.get(key))
        return existing;
    Structure* created = Object::createStructure(vm, instance, prototype, &layoutOfShape(shape));
    m_structuresWithOtherPrototype.set(key, created);
    return created;
}

void State::fill(VM& vm, uint32_t number)
{
    const AllocationData& allocation = this->allocation(number);
    Structure* structure = this->structure(vm, allocation.shape);
    if (allocation.constructedBy != LayoutData::noBase) {
        structure = structureFor(vm, allocation.shape, prototypeForObjectsOf(vm, closure(vm, allocation.constructedBy)));
        didCreate(structure);
    }
    size_t size = structure->compiledLayout() ? allocation.size : Closure::allocationSize();
    auto& slot = instance.typedAllocations[number];
    slot.header = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
    slot.allocator = vm.cellSpace().allocatorFor(size, AllocatorForMode::EnsureAllocator).localAllocator();
    slot.words = static_cast<uint64_t>(shapeWordOf(allocation.shape)) << 32 | allocation.orderWord;
}

JSCell* State::allocate(VM& vm, uint32_t number)
{
    auto& slot = instance.typedAllocations[number];
    if (!slot.header)
        fill(vm, number);
    Structure* structure = std::bit_cast<StructureID>(static_cast<uint32_t>(slot.header)).decode();
    if (!structure->compiledLayout())
        return Closure::create(vm, structure, 0, nullptr, 0);
    return Object::create(vm, structure, allocation(number).size, allocation(number).orderWord);
}

uint32_t State::sizeWithRoomToSpecialize(uint32_t shape)
{
    uint32_t& size = m_sizesWithRoomToSpecialize[shape];
    if (size)
        return size;
    BitVector seen;
    seen.set(shape);
    Vector<uint32_t, 8> worklist { shape };
    while (!worklist.isEmpty()) {
        const LayoutData& layout = layoutOfShape(worklist.takeLast());
        size = std::max(size, layout.size);
        for (uint32_t next : std::span { data.at<uint32_t>(layout.specializationsOffset), layout.numberOfSpecializations }) {
            if (!seen.set(next))
                worklist.append(next);
        }
    }
    return size;
}

bool State::hasFixedMethod(const LayoutData& layout, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return false;
    bool isDeclared = false;
    for (const LayoutData* next = &layout; next && !isDeclared; next = baseOf(*next)) {
        if (isFixedIn(next->methods(), name, isDeclared))
            return true;
    }
    return false;
}

bool State::hasPrototypeMembers(const LayoutData& layout) const
{
    for (const LayoutData* next = &layout; next; next = baseOf(*next)) {
        if (next->numberOfMethods)
            return true;
    }
    return false;
}

// `#name() {}`, `get #name() {}` of a class that is evaluated once: every object of the class has it, and of a class that extends it. The
// name has been asked for by name().
bool State::findPrivateMethod(VM& vm, const LayoutData& layout, UniquedStringImpl* name, Object::PrivateElement& element)
{
    for (const LayoutData* next = &layout; next; next = baseOf(*next)) {
        for (auto& method : next->methods()) {
            if (!next->isEvaluatedOnce || method.flags & methodHasComputedName || names[method.name].impl() != name)
                continue;
            switch (static_cast<Format::MethodKind>(method.kind)) {
            case Format::MethodKind::Method:
                element.kind = Object::PrivateElement::Kind::Method;
                element.value = closure(vm, method.function);
                break;
            case Format::MethodKind::Getter:
                element.kind = Object::PrivateElement::Kind::Accessor;
                element.getter = closure(vm, method.function);
                break;
            case Format::MethodKind::Setter:
                element.kind = Object::PrivateElement::Kind::Accessor;
                element.setter = closure(vm, method.function);
                break;
            }
        }
        if (element.kind != Object::PrivateElement::Kind::None)
            return true;
    }
    return false;
}

static constexpr uint64_t hashOfNoFields = 0xcbf29ce484222325ull;

static uint64_t hashWithField(uint64_t hash, uint16_t field)
{
    return (hash ^ (field + 1)) * 0x100000001b3ull;
}

// The two greatest numbers are no keys.
static uint64_t keyOf(uint64_t hash)
{
    return hash >> 1;
}

std::span<const uint16_t> State::fieldsInOrder(const LayoutData& layout, uint32_t order)
{
    if (order >= layout.numberOfOrders)
        return ordersOf(layout).added[order - layout.numberOfOrders].span();
    const uint16_t* lengthAndFields = data.at<uint16_t>(data.at<uint32_t>(layout.ordersOffset)[order]);
    return { lengthAndFields + 1, lengthAndFields[0] };
}

State::OrderTable& State::ordersOf(const LayoutData& layout)
{
    OrderTable*& table = m_orderTables[layout.shape];
    if (table) [[likely]]
        return *table;
    auto created = makeUnique<OrderTable>();
    table = created.get();
    m_createdOrderTables.append(WTF::move(created));
    RELEASE_ASSERT(layout.numberOfOrders < keysAreInLayoutOrder);
    for (uint16_t order = 0; order < layout.numberOfOrders; ++order) {
        uint64_t hash = hashOfNoFields;
        table->byPrefix.add(keyOf(hash), order);
        for (uint16_t field : fieldsInOrder(layout, order)) {
            hash = hashWithField(hash, field);
            table->byPrefix.add(keyOf(hash), order);
        }
    }
    return *table;
}

uint32_t State::orderStartingWith(const LayoutData& layout, std::span<const uint16_t> fields)
{
    static constexpr uint32_t noOrder = orderWordOf(keysAreInLayoutOrder, 0);
    if (fields.size() > maxFieldsInOrder)
        return noOrder;
    OrderTable& table = ordersOf(layout);
    Vector<uint64_t, 16> hashes { hashOfNoFields };
    for (uint16_t field : fields)
        hashes.append(hashWithField(hashes.last(), field));
    auto find = [&](size_t length) -> std::optional<uint16_t> {
        auto found = table.byPrefix.find(keyOf(hashes[length]));
        if (found == table.byPrefix.end())
            return std::nullopt;
        auto all = fieldsInOrder(layout, found->value);
        if (all.size() < length || !equalSpans(all.first(length), fields.first(length)))
            return std::nullopt;
        return found->value;
    };
    if (auto order = find(fields.size()))
        return orderWordOf(*order, fields.size());
    // An order that has come up when the program ran, and ends where this one goes on, is made longer.
    if (auto order = fields.empty() ? std::nullopt : find(fields.size() - 1); order && *order >= layout.numberOfOrders) {
        auto& all = table.added[*order - layout.numberOfOrders];
        if (all.size() + 1 == fields.size()) {
            all.append(fields.back());
            table.byPrefix.add(keyOf(hashes.last()), *order);
            return orderWordOf(*order, fields.size());
        }
    }
    uint32_t order = layout.numberOfOrders + table.added.size();
    if (order >= keysAreInLayoutOrder)
        return noOrder;
    table.added.append(Vector<uint16_t>(fields));
    for (uint64_t hash : hashes)
        table.byPrefix.add(keyOf(hash), static_cast<uint16_t>(order));
    return orderWordOf(order, fields.size());
}

uint32_t State::orderAfterCreating(const LayoutData& layout, uint32_t orderWord, uint16_t field)
{
    uint32_t order = orderOf(orderWord);
    uint32_t count = countOf(orderWord);
    if (order == keysAreInLayoutOrder)
        return orderWord & orderAndCountMask;
    auto fields = fieldsInOrder(layout, order);
    if (count < fields.size() && fields[count] == field) [[likely]]
        return orderWordOf(order, count + 1);
    OrderTable& table = ordersOf(layout);
    uint64_t key = static_cast<uint64_t>(order) << 32 | count << 16 | field;
    if (auto found = table.transitions.find(key); found != table.transitions.end())
        return orderWordOf(found->value, found->value == keysAreInLayoutOrder ? 0 : count + 1);
    Vector<uint16_t, 16> next(fields.first(count));
    next.append(field);
    uint32_t result = orderStartingWith(layout, next.span());
    table.transitions.add(key, static_cast<uint16_t>(orderOf(result)));
    return result;
}

// What becomes of an object that is cast. A plain data object is copied into the layout of the type, through its properties and the elements of arrays. An object
// of the program becomes one of a specialization of its layout where it is. Nothing the program can see is changed before all of it is
// known to work out, and no code of the program runs.
//
// A type that admits foreign objects has plain objects in its places as they are, so one more is not copied: it would be two from then
// on. It stays itself, and what its properties hold is coerced where it is, like the elements of an array. An object that nothing can be
// made of is of such a type as it is, where for any other type the cast fails.
//
// Where a value is narrowed, by a type guard or a discriminant, only the second of these is done: nothing is copied, and no array and no
// plain object gets anything stored to it. Compiled code counts on that: see narrowingMayRetag().
class Coercion {
public:
    enum class Mode : uint8_t { Cast, Narrowing };

    Coercion(JSGlobalObject* globalObject, State& state, Mode mode = Mode::Cast)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_state(state)
        , m_mode(mode)
    {
    }

    // Empty: it cannot be done, see failure(), or an exception has been thrown.
    JSValue run(JSValue value, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue result = coerce(value, type);
        RETURN_IF_EXCEPTION(scope, { });
        if (!result)
            return { };
        for (auto& specialization : m_specializations) {
            Object* object = specialization.object;
            object->specialize(m_vm, m_state, specialization.shape);
            const LayoutData& layout = object->layout();
            for (auto& [index, moved] : specialization.movedIntoFields) {
                const FieldData& field = layout.fields()[index];
                const Identifier& name = m_state.name(m_vm, field.name);
                object->didCreateField(m_state, field);
                bool didStore = object->store(m_vm, field, moved);
                RELEASE_ASSERT(didStore);
                JSObject* extra = object->extraProperties();
                DeletePropertySlot slot;
                extra->methodTable()->deleteProperty(extra, m_globalObject, name, slot);
                scope.assertNoException();
            }
            object->dropExtraPropertiesIfEmpty(m_vm);
        }
        for (auto& store : m_elementStores) {
            store.array->putDirectIndex(m_globalObject, store.index, store.value);
            RETURN_IF_EXCEPTION(scope, { });
        }
        // They are writable data properties: there is no transition.
        for (auto& store : m_propertyStores)
            store.object->putDirect(m_vm, store.name, store.value);
        RELEASE_ASSERT(m_state.fits(type, result));
        return result;
    }

    const String& failure() const { return m_failure; }

private:
    struct Property {
        UniquedStringImpl* name;
        PropertyOffset offset;
        unsigned attributes;
    };
    using Properties = Vector<Property, 16>;
    struct Specialization {
        Object* object;
        uint32_t shape;
        // Extra properties by the names of fields that the shape has more.
        Vector<std::pair<uint16_t, JSValue>, 1> movedIntoFields;
    };
    struct ElementStore {
        JSArray* array;
        unsigned index;
        JSValue value;
    };
    struct PropertyStore {
        JSObject* object;
        UniquedStringImpl* name;
        JSValue value;
    };
    // How far a plain object is from a layout it can be made one of. The less the better, in this order.
    struct Distance {
        // Properties that hold none of the strings, or of the numbers, that are all the type of the field allows.
        unsigned otherLiterals { 0 };
        // Properties the layout has no field for.
        unsigned extraProperties { 0 };

        friend auto operator<=>(const Distance&, const Distance&) = default;
    };
    // A name, or an index if that is null.
    struct Step {
        UniquedStringImpl* name;
        unsigned index;
    };

    uint32_t shapeOf(Object* object) const
    {
        auto planned = m_plannedShapes.find(object);
        return planned == m_plannedShapes.end() ? object->shape() : planned->value;
    }

    // Whether there is nothing to make of the value.
    bool fits(JSValue value, const RuntimeType& type) const
    {
        if (Object* object = m_state.compiledObject(value))
            return type.allowsAnyObject || m_state.admitsShape(type, shapeOf(object));
        return value.isObject() ? m_state.fitsAsOneOfItsOwn(type, asObject(value)) : m_state.fits(type, value);
    }

    // Nothing is left of an attempt that has failed.
    JSValue orAsItIs(JSValue value, const RuntimeType& type, const Invocable<JSValue()> auto& attempt)
    {
        if (!type.admitsForeignObjects)
            return attempt();
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        size_t numberOfCopies = m_copied.size();
        size_t numberOfArrays = m_arraysInOrder.size();
        size_t numberOfSpecializations = m_specializations.size();
        size_t numberOfElementStores = m_elementStores.size();
        size_t numberOfKeptObjects = m_keptInOrder.size();
        size_t numberOfPropertyStores = m_propertyStores.size();
        size_t lengthOfPath = m_path.size();
        JSValue result = attempt();
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
        for (size_t i = numberOfCopies; i < m_copied.size(); ++i)
            m_copies.remove(m_copied[i]);
        m_copied.shrink(numberOfCopies);
        for (size_t i = numberOfArrays; i < m_arraysInOrder.size(); ++i)
            m_arrays.remove(m_arraysInOrder[i]);
        m_arraysInOrder.shrink(numberOfArrays);
        m_specializations.shrink(numberOfSpecializations);
        m_plannedShapes.clear();
        for (auto& specialization : m_specializations)
            m_plannedShapes.set(specialization.object, specialization.shape);
        m_elementStores.shrink(numberOfElementStores);
        for (size_t i = numberOfKeptObjects; i < m_keptInOrder.size(); ++i)
            m_kept.remove(m_keptInOrder[i]);
        m_keptInOrder.shrink(numberOfKeptObjects);
        m_propertyStores.shrink(numberOfPropertyStores);
        m_path.shrink(lengthOfPath);
        m_failure = String();
        return value;
    }

    JSValue fail(const String& what)
    {
        if (!m_failure.isNull())
            return { };
        StringBuilder path;
        for (auto& step : m_path) {
            if (step.name)
                path.append(path.isEmpty() ? ""_s : "."_s, StringView(step.name));
            else
                path.append('[', step.index, ']');
        }
        m_failure = path.isEmpty() ? what : makeString("property '"_s, path.toString(), "': "_s, what);
        return { };
    }

    JSValue failToFit(JSValue value, const RuntimeType& type)
    {
        return fail(makeString(State::describe(value), " is not of type "_s, m_state.describe(type)));
    }

    JSValue coerce(JSValue value, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!value.isObject())
            return m_state.fits(type, value) ? value : failToFit(value, type);
        if (!m_vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(m_globalObject, scope);
            return { };
        }
        if (Object* object = m_state.compiledObject(value)) {
            if (fits(value, type))
                return value;
            RELEASE_AND_RETURN(scope, orAsItIs(value, type, [&] {
                return specialize(object, type);
            }));
        }
        if (m_mode == Mode::Narrowing)
            return m_state.fits(type, value) ? value : failToFit(value, type);
        if (fits(value, type)) {
            if (!type.elements || !isJSArray(value))
                return value;
            bool didCoerce = coerceElements(asArray(value), *m_state.data.at<RuntimeType>(type.elements));
            RETURN_IF_EXCEPTION(scope, { });
            return didCoerce ? value : JSValue();
        }
        JSObject* object = asObject(value);
        if (Object* copy = m_copies.get(object))
            return fits(copy, type) || type.admitsForeignObjects ? JSValue(copy) : failToFit(copy, type);
        if (type.admitsForeignObjects && !isPlainObject(object))
            return value;
        RELEASE_AND_RETURN(scope, orAsItIs(value, type, [&] {
            return type.admitsForeignObjects ? keep(object, type) : copy(object, type);
        }));
    }

    // Whether the value is one that coerce() may make something of: for choosing among the parts of a union.
    bool mayBeCoerced(JSValue value, const RuntimeType& type)
    {
        if (fits(value, type))
            return true;
        if (!value.isObject())
            return false;
        if (type.admitsForeignObjects)
            return true;
        if (Object* object = m_state.compiledObject(value)) {
            bool hasSpecialization = false;
            forEachSpecialization(shapeOf(object), type, [&](const LayoutData&) {
                hasSpecialization = true;
                return IterationStatus::Done;
            });
            return hasSpecialization;
        }
        if (m_mode == Mode::Narrowing)
            return false;
        if (Object* copy = m_copies.get(asObject(value)))
            return fits(copy, type);
        if (asObject(value)->type() != FinalObjectType)
            return false;
        bool hasLayout = false;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData&) {
            hasLayout = true;
            return IterationStatus::Done;
        });
        return hasLayout;
    }

    // What an object may be made one of so that it is of the type: the layout of each view. Nothing is made as what the parts of a large
    // union have in common, which is one view: there it is the members.
    void forEachLayoutToMakeObjectsOf(const RuntimeType& type, const Invocable<IterationStatus(const LayoutData&)> auto& functor) const
    {
        auto isDoneWith = [&](uint32_t shape) {
            const LayoutData& layout = m_state.layoutOfShape(shape);
            return layout.kind == LayoutKind::Record && functor(layout) == IterationStatus::Done;
        };
        for (uint32_t i = 0; i < type.numberOfViews; ++i) {
            auto& view = type.views()[i];
            if (view.shapeOfView) {
                if (isDoneWith(view.shapeOfView))
                    return;
                continue;
            }
            for (uint32_t shape = view.first; shape < view.first + view.count; ++shape) {
                if (m_state.passes(view, shape) && isDoneWith(shape))
                    return;
            }
        }
    }

    // Of what the type offers, what an object of the shape can be made one of where it is.
    void forEachSpecialization(uint32_t shape, const RuntimeType& type, const Invocable<IterationStatus(const LayoutData&)> auto& functor) const
    {
        const LayoutData& layout = m_state.layoutOfShape(shape);
        // An object of a class keeps its prototype. But for what it finds there compiled code goes by the shape, which it loses.
        if (!layout.numberOfSpecializations || m_state.hasPrototypeMembers(layout))
            return;
        std::span specializations { m_state.data.at<uint32_t>(layout.specializationsOffset), layout.numberOfSpecializations };
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& candidate) {
            return std::ranges::binary_search(specializations, candidate.shape) ? functor(candidate) : IterationStatus::Continue;
        });
    }

    // As distanceTo() for a plain object. What the object has in its fields stays there whatever it is, so that rules nothing out.
    // Nothing: an extra property cannot become the field that the layout has by its name.
    std::optional<Distance> distanceTo(Object* object, const Properties& extraProperties, const LayoutData& layout)
    {
        Distance distance;
        const LayoutData& current = object->layout();
        for (uint32_t i = 0; i < current.numberOfFields; ++i) {
            JSValue value = object->read(current.fields()[i]);
            const FieldData* field = value ? m_state.field(m_vm, layout, m_state.name(m_vm, current.fields()[i].name).impl()) : nullptr;
            if (field)
                distance.otherLiterals += !fits(value, m_state.ownTypeOf(*field)) || !m_state.isAmongLiterals(*field, value);
        }
        for (auto& property : extraProperties) {
            const FieldData* field = m_state.field(m_vm, layout, property.name);
            if (!field) {
                ++distance.extraProperties;
                continue;
            }
            JSValue value = object->extraProperties()->getDirect(property.offset);
            if (property.attributes || !mayBeCoerced(value, m_state.ownTypeOf(*field)))
                return std::nullopt;
            distance.otherLiterals += !m_state.isAmongLiterals(*field, value);
        }
        return distance;
    }

    // Of the parts of a union, as chooseLayout() has it. Null: there is none.
    const LayoutData* chooseSpecialization(Object* object, const RuntimeType& type)
    {
        Vector<const LayoutData*, 4> candidates;
        forEachSpecialization(shapeOf(object), type, [&](const LayoutData& candidate) {
            candidates.append(&candidate);
            return IterationStatus::Continue;
        });
        if (candidates.size() < 2)
            return candidates.isEmpty() ? nullptr : candidates[0];
        Properties extraProperties;
        if (JSObject* extra = object->extraProperties())
            extraProperties = propertiesOf(extra);
        // If none will do, specialize() says why the first will not.
        const LayoutData* layout = nullptr;
        Distance least;
        for (const LayoutData* candidate : candidates) {
            if (auto distance = distanceTo(object, extraProperties, *candidate); distance && (!layout || *distance < least)) {
                least = *distance;
                layout = candidate;
            }
        }
        return layout ? layout : candidates[0];
    }

    // In the order of their creation.
    Properties propertiesOf(JSObject* object)
    {
        Properties properties;
        object->structure()->forEachProperty(m_vm, [&](const auto& entry) {
            properties.append({ entry.key(), entry.offset(), entry.attributes() });
            return true;
        });
        return properties;
    }

    JSValue specialize(Object* object, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        uint32_t from = shapeOf(object);
        const LayoutData* chosen = chooseSpecialization(object, type);
        if (!chosen)
            return failToFit(object, type);
        const LayoutData& layout = *chosen;
        uint32_t to = layout.shape;
        // Only stores to layouts that may be frozen look at what the object has been made.
        if (object->isNotExtensible() && !layout.mayBeFrozen)
            return fail(makeString("it has been frozen, sealed or kept from being extended, which the program is not seen to do to objects of type "_s, m_state.nameOf(layout)));
        // First, for what refers back to the object.
        m_plannedShapes.set(object, to);
        size_t index = m_specializations.size();
        m_specializations.append({ object, to, { } });
        if (JSObject* extra = object->extraProperties()) {
            // As they were created, which is the order they keep as fields.
            for (auto& property : propertiesOf(extra)) {
                const FieldData* field = m_state.field(m_vm, layout, property.name);
                if (!field || m_state.field(m_vm, m_state.layoutOfShape(from), property.name))
                    continue;
                m_path.append({ property.name, 0 });
                if (property.attributes)
                    return fail("it is not a writable, enumerable and configurable data property"_s);
                JSValue coerced = coerce(extra->getDirect(property.offset), m_state.ownTypeOf(*field));
                RETURN_IF_EXCEPTION(scope, { });
                if (!coerced)
                    return { };
                m_path.removeLast();
                m_specializations[index].movedIntoFields.append(std::pair { static_cast<uint16_t>(field - layout.fields()), coerced });
            }
        }
        return object;
    }

    bool coerceElements(JSArray* array, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!type.numberOfViews && !type.elements)
            return true;
        unsigned length = array->length();
        unsigned i = 0;
        // An array whose elements all fit costs this pass.
        while (!type.elements && i < length && (!array->canGetIndexQuickly(i) || fits(array->getIndexQuickly(i), type)))
            ++i;
        if (i == length || !m_arrays.add({ array, &type }).isNewEntry)
            return true;
        m_arraysInOrder.append({ array, &type });
        for (; i < length; ++i) {
            // Not a hole, nor what is kept aside, which may be an accessor: such an element is tested when it is read.
            if (!array->canGetIndexQuickly(i))
                continue;
            JSValue element = array->getIndexQuickly(i);
            m_path.append({ nullptr, i });
            JSValue coerced = coerce(element, type);
            RETURN_IF_EXCEPTION(scope, false);
            if (!coerced)
                return false;
            m_path.removeLast();
            if (coerced != element)
                m_elementStores.append({ array, i, coerced });
        }
        return true;
    }

    // An ordinary object that inherits from Object.prototype or from nothing, and can be extended. Its properties are looked at one by one.
    bool isPlainObject(JSObject* object) const
    {
        if (object->type() != FinalObjectType || !object->isStructureExtensible())
            return false;
        JSValue prototype = object->getPrototypeDirect();
        return prototype.isNull() || prototype == JSValue(object->realm()->objectPrototype());
    }

    // Nothing: the object cannot be made one of the layout. With `explains`, says why. It goes by what the type of the layout says of
    // each property: the slot is shared with the other parts of a union, and holds what any of them does.
    std::optional<Distance> distanceTo(JSObject* object, const Properties& properties, const LayoutData& layout, bool explains)
    {
        Distance distance;
        BitVector isPresent;
        for (auto& property : properties) {
            const FieldData* field = m_state.field(m_vm, layout, property.name);
            if (!field) {
                ++distance.extraProperties;
                continue;
            }
            isPresent.set(field - layout.fields());
            JSValue value = object->getDirect(property.offset);
            if (!property.attributes && mayBeCoerced(value, m_state.ownTypeOf(*field))) {
                distance.otherLiterals += !m_state.isAmongLiterals(*field, value);
                continue;
            }
            if (explains) {
                m_path.append({ property.name, 0 });
                if (property.attributes)
                    fail("it is not a writable, enumerable and configurable data property"_s);
                else
                    failToFit(value, m_state.ownTypeOf(*field));
            }
            return std::nullopt;
        }
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            if (layout.fields()[i].flags & fieldIsOptional || isPresent.get(i))
                continue;
            if (explains) {
                m_path.append({ m_state.name(m_vm, layout.fields()[i].name).impl(), 0 });
                fail(makeString("it is missing, and type "_s, m_state.nameOf(layout), " requires it"_s));
            }
            return std::nullopt;
        }
        return distance;
    }

    static bool hasCustomProperties(const Properties& properties)
    {
        return properties.containsIf([](auto& property) {
            return property.attributes & PropertyAttribute::CustomAccessorOrValue;
        });
    }

    // Of the parts of a union the one whose discriminants the object has. A discriminant that no part knows rules none of them out: the
    // program may well look at it and go on. Then the one that leaves the fewest properties without a field, and of those the first.
    // Null: there is none. With `explains`, says why the first is not it.
    const LayoutData* chooseLayout(JSObject* object, const Properties& properties, const RuntimeType& type, bool explains)
    {
        const LayoutData* layout = nullptr;
        const LayoutData* firstCandidate = nullptr;
        Distance least;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& candidate) {
            if (!firstCandidate)
                firstCandidate = &candidate;
            if (auto distance = distanceTo(object, properties, candidate, false); distance && (!layout || *distance < least)) {
                least = *distance;
                layout = &candidate;
            }
            return IterationStatus::Continue;
        });
        if (!layout && firstCandidate && explains)
            distanceTo(object, properties, *firstCandidate, true);
        return layout;
    }

    // The object stays what it is. What its properties hold becomes what the fields of the layout it would be copied into hold.
    JSValue keep(JSObject* object, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!m_kept.add({ object, &type }).isNewEntry)
            return object;
        m_keptInOrder.append({ object, &type });
        Properties properties = propertiesOf(object);
        const LayoutData* layout = hasCustomProperties(properties) ? nullptr : chooseLayout(object, properties, type, false);
        if (!layout)
            return object;
        for (auto& property : properties) {
            const FieldData* field = m_state.field(m_vm, *layout, property.name);
            if (!field)
                continue;
            JSValue value = object->getDirect(property.offset);
            m_path.append({ property.name, 0 });
            JSValue coerced = coerce(value, m_state.ownTypeOf(*field));
            RETURN_IF_EXCEPTION(scope, { });
            if (!coerced)
                return { };
            m_path.removeLast();
            if (coerced != value)
                m_propertyStores.append({ object, property.name, coerced });
        }
        return object;
    }

    JSValue copy(JSObject* object, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!isPlainObject(object))
            return failToFit(object, type);
        // A getter and a setter are copied as what they are, which runs neither. No field holds them: see distanceTo().
        Properties properties = propertiesOf(object);
        if (hasCustomProperties(properties))
            return fail("an object with a property that the engine computes cannot be copied"_s);
        const LayoutData* layout = chooseLayout(object, properties, type, true);
        if (!layout)
            return failToFit(object, type);

        Structure* structure = m_state.structureFor(m_vm, layout->shape, object->getPrototypeDirect());
        Object* result = Object::create(m_vm, structure, m_state.sizeWithRoomToSpecialize(layout->shape), orderWordOf(keysAreInLayoutOrder, 0));
        m_copiesKeptAlive.append(result);
        if (m_copiesKeptAlive.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(m_globalObject, scope);
            return { };
        }
        m_copies.add(object, result);
        m_copied.append(object);
        Vector<uint16_t, 16> order;
        for (auto& property : properties) {
            JSValue value = object->getDirect(property.offset);
            const FieldData* field = m_state.field(m_vm, *layout, property.name);
            if (!field) {
                result->willAddExtraProperty(m_vm, property.name);
                if (property.attributes & PropertyAttribute::Accessor)
                    result->ensureExtraProperties(m_vm)->putDirectAccessor(m_globalObject, property.name, uncheckedDowncast<GetterSetter>(value), property.attributes);
                else
                    result->ensureExtraProperties(m_vm)->putDirect(m_vm, property.name, value, property.attributes);
                continue;
            }
            m_path.append({ property.name, 0 });
            JSValue coerced = coerce(value, m_state.ownTypeOf(*field));
            RETURN_IF_EXCEPTION(scope, { });
            if (!coerced)
                return { };
            if (!result->store(m_vm, *field, coerced))
                return failToFit(value, m_state.ownTypeOf(*field));
            m_path.removeLast();
            order.append(field - layout->fields());
        }
        if (hasIndexedProperties(object->indexingType())) {
            PropertyNameArrayBuilder indices(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            object->getOwnIndexedPropertyNames(m_globalObject, indices, DontEnumPropertiesMode::Include);
            for (auto& name : indices) {
                uint32_t index = *parseIndex(name);
                PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                if (!JSObject::getOwnPropertySlotByIndex(object, m_globalObject, index, slot) || !slot.isValue() || slot.attributes() & PropertyAttribute::ReadOnly)
                    return fail("an object with an indexed accessor or read-only element cannot be copied"_s);
                result->ensureExtraProperties(m_vm)->putDirectIndex(m_globalObject, index, slot.getValue(m_globalObject, index), slot.attributes(), PutDirectIndexLikePutDirect);
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        result->setOrderAndCount(m_state.orderStartingWith(*layout, order.span()));
        return result;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    State& m_state;
    Mode m_mode;
    // What is reached twice is copied once, so that what was one object is one object, and a cycle a cycle.
    UncheckedKeyHashMap<JSObject*, Object*> m_copies;
    MarkedArgumentBuffer m_copiesKeptAlive;
    UncheckedKeyHashMap<Object*, uint32_t> m_plannedShapes;
    UncheckedKeyHashSet<std::pair<JSArray*, const RuntimeType*>> m_arrays;
    UncheckedKeyHashSet<std::pair<JSObject*, const RuntimeType*>> m_kept;
    // The keys of m_copies, m_arrays and m_kept as they came, for orAsItIs().
    Vector<JSObject*> m_copied;
    Vector<std::pair<JSArray*, const RuntimeType*>> m_arraysInOrder;
    Vector<std::pair<JSObject*, const RuntimeType*>> m_keptInOrder;
    Vector<Specialization> m_specializations;
    Vector<ElementStore> m_elementStores;
    Vector<PropertyStore> m_propertyStores;
    Vector<Step, 8> m_path;
    String m_failure;
};

JSValue State::coerce(JSGlobalObject* globalObject, JSValue value, const RuntimeType& type, Format::CastReason reason, uint32_t function, uint32_t position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasObjects = type.allowsAnyObject || type.allowsAnyFunction || type.admitsForeignObjects || type.numberOfViews || type.numberOfJSTypes;
    if (reason == Format::CastReason::Coerce && !(value.isObject() && hasObjects))
        RELEASE_AND_RETURN(scope, convert(globalObject, value, type, function, position));
    bool isNarrowing = reason == Format::CastReason::Flow;
    Coercion coercion(globalObject, *this, isNarrowing ? Coercion::Mode::Narrowing : Coercion::Mode::Cast);
    JSValue result = coercion.run(value, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    // An object that nothing can be made of stays what it is.
    if (reason == Format::CastReason::Coerce) {
        if (Options::reportAOTTypedCoercions()) [[unlikely]]
            reportCoercion(function, position, makeString(describe(value), " is let through as "_s, describe(type), coercion.failure().isEmpty() ? ""_s : ": "_s, coercion.failure()));
        return value;
    }
    if (isNarrowing) {
        // As operationAOTTypedThrowTypeMismatch has it.
        throwTypeError(globalObject, scope, makeString(describe(value), " is not of type "_s, describe(type), describePosition(function, position)));
        return { };
    }
    throwTypeError(globalObject, scope, makeString("Cast of "_s, describe(value), " to type "_s, describe(type), " failed: "_s, coercion.failure(), describePosition(function, position)));
    return { };
}

// `value as T` where the value is no object, or T has none: see Format::CastReason::Coerce.
JSValue State::convert(JSGlobalObject* globalObject, JSValue value, const RuntimeType& type, uint32_t function, uint32_t position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (fits(type, value))
        return value;
    bool hasNumber = type.primitives & Primitive::Number;
    bool hasString = type.primitives & Primitive::String;
    bool hasBoolean = type.primitives & Primitive::Boolean;
    bool hasBigInt = type.primitives & Primitive::BigInt;
    bool hasSeveralKinds = hasNumber + hasString + hasBoolean + hasBigInt + !!(type.primitives & Primitive::Symbol) > 1;
    JSValue result = value;
    if (hasSeveralKinds) {
        result = value.toPrimitive(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        hasBigInt = false;
    }
    if (!fits(type, result)) {
        if (hasString)
            result = result.toString(globalObject);
        else if (hasNumber)
            result = jsNumber(result.toNumber(globalObject));
        else if (hasBoolean)
            result = jsBoolean(result.toBoolean(globalObject));
        else if (hasBigInt)
            result = result.toBigInt(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    // There is no conversion to a symbol or to an object, and a type may have room for some numbers only, or for one boolean.
    if (!fits(type, result)) {
        throwTypeError(globalObject, scope, makeString(describe(value), " is not of type "_s, describe(type), describePosition(function, position)));
        return { };
    }
    if (Options::reportAOTTypedCoercions()) [[unlikely]]
        reportCoercion(function, position, makeString(describe(value), " is converted to "_s, describe(result), " for "_s, describe(type)));
    return result;
}

void State::reportCoercion(uint32_t function, uint32_t position, const String& what)
{
    if (m_reportedCoercions.add(static_cast<uint64_t>(function) << 32 | position).isNewEntry)
        dataLogLn("AOT:", describePosition(function, position), ": ", what);
}

JSValue State::holdToTypeOf(JSGlobalObject* globalObject, const FieldData& field, JSValue value, String& failure)
{
    const RuntimeType& type = ownTypeOf(field);
    if (!(field.flags & fieldIsAnnotated) || !value.isObject())
        return fits(type, value) ? value : JSValue();
    Coercion coercion(globalObject, *this);
    JSValue result = coercion.run(value, type);
    if (!result)
        failure = coercion.failure();
    return result;
}

uint64_t State::entry(uint32_t function)
{
    uint64_t& entry = instance.typedFunctions[function];
    if (!entry) {
        uint64_t relative = instance.program->data().typedEntry(function);
        RELEASE_ASSERT(relative);
        entry = (relative & ~EntryWord::addressMask) | (std::bit_cast<uintptr_t>(instance.code) + (relative & EntryWord::addressMask));
        if (kindOf(function) == Format::FunctionKind::Constructor)
            entry |= 1ULL << EntryWord::isNotCallableBit;
    }
    return entry;
}

Closure* State::closure(VM& vm, uint32_t function)
{
    JSCell*& cell = instance.typedClosures[function];
    if (!cell) {
        cell = Closure::create(vm, closureStructure(vm, function), entry(function), nullptr, function);
        didCreate(cell);
        // The class object of a class that extends something foreign is made at the declaration, where that is evaluated.
        if (const LayoutData* ofClass = classConstructedBy(function); ofClass && !ofClass->hasForeignRoot) {
            RELEASE_ASSERT(ofClass->isEvaluatedOnce);
            makeClass(instance.globalObject, uncheckedDowncast<Closure>(cell), JSValue());
        }
    }
    return uncheckedDowncast<Closure>(cell);
}

template<typename Visitor>
void State::visit(Visitor& visitor)
{
    {
        Locker locker { m_cellsLock };
        for (JSCell* cell : m_cells)
            visitor.appendUnbarriered(cell);
    }
    for (JSCell* cell : std::initializer_list<JSCell*> { moduleRecord, importMeta, m_closureStructures[0], m_closureStructures[1], m_closureStructures[2], m_closureStructures[3], m_liveBindingGetter }) {
        if (cell)
            visitor.appendUnbarriered(cell);
    }
    const uint32_t* roots = data.at<uint32_t>(data.rootsOffset);
    for (uint32_t i = 0; i < data.numberOfRoots; ++i)
        visitor.appendUnbarriered(JSValue::decode(*reinterpret_cast<EncodedJSValue*>(instance.typedStatics + roots[i])));
}

template void State::visit(AbstractSlotVisitor&);
template void State::visit(SlotVisitor&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
