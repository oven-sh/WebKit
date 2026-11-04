/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyObject.h"

#include "JSCInlines.h"
#include "JSObjectVisitInlines.h"
#include "PyDict.h"
#include "PyJSView.h"
#include "PyRuntime.h"
#include "PyStr.h"
#include "PyTuple.h"
#include "PyType.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// ---- PyObject ----

const ClassInfo PyObject::s_info = { "PyObject"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyObject) };

PyObject::PyObject(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure)
    , m_type(type, WriteBarrierEarlyInit)
{
}

Structure* PyObject::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, prototypeOfPythonObjects(globalObject), TypeInfo(PyObjectType, StructureFlags), info());
}

PyObject* PyObject::create(VM& vm, Structure* structure, PyType* type)
{
    auto* object = new (NotNull, allocateCell<PyObject>(vm)) PyObject(vm, structure, type);
    object->finishCreation(vm);
    return object;
}

void PyObject::setType(VM& vm, PyType* type)
{
    m_type.set(vm, this, type);
}

CallData PyObject::getCallData(JSCell* cell)
{
    CallData callData;
    if (uncheckedDowncast<PyObject>(cell)->type()->lookup(Special::Call)) {
        callData.type = CallData::Type::Native;
        callData.native.function = callPythonObject;
        callData.native.isBoundFunction = false;
        callData.native.isWasm = false;
    }
    return callData;
}

// What can stand among the bases of a class (Generic[T], list[int]) can stand behind `extends`, which takes constructors only.
CallData PyObject::getConstructData(JSCell* cell)
{
    CallData constructData;
    if (uncheckedDowncast<PyObject>(cell)->type()->lookup(Special::MroEntries)) {
        constructData.type = CallData::Type::Native;
        constructData.native.function = callPythonObject;
        constructData.native.isBoundFunction = false;
        constructData.native.isWasm = false;
    }
    return constructData;
}

PY_DEFINE_JS_VIEW(PyObject)

JSObject* prototypeOfPythonObjects(JSGlobalObject* globalObject)
{
    return runtime(globalObject).jsViewObject(JSViewObject::ObjectPrototype);
}

template<typename Visitor>
void PyObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyObject>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_type);
}

DEFINE_VISIT_CHILDREN(PyObject);

// ---- PyInstance ----

PY_DEFINE_CELL(PyInstance);

static_assert(PyObject::offsetOfType() == JSObject::offsetOfInlineStorage());
static_assert(PyInstance::offsetOfDict() == JSObject::offsetOfInlineStorage() + sizeof(EncodedJSValue));
static_assert(sizeof(PyInstance) == JSObject::offsetOfInlineStorage() + PyInstance::numberOfFields * sizeof(EncodedJSValue));

PyInstance::PyInstance(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
    memset(inlineStorageUnsafe() + numberOfFields, 0, (structure->inlineCapacity() - numberOfFields) * sizeof(EncodedJSValue));
}

// With room in the object itself for so many properties, the fields included. For PyType::createInstanceStructure().
Structure* createStructureOfInstances(VM& vm, JSGlobalObject* globalObject, Runtime& rt, unsigned inlineCapacity)
{
    constexpr unsigned numberOfFields = PyInstance::numberOfFields;
    Structure* structure = Structure::create(vm, globalObject, prototypeOfPythonObjects(globalObject), TypeInfo(PyInstanceType, PyInstance::StructureFlags), PyInstance::info(), NonArray, inlineCapacity);
    for (unsigned i = 0; i < numberOfFields; ++i) {
        PropertyOffset offset;
        structure = Structure::addPropertyTransition(vm, structure, rt.instanceFieldKey(i), static_cast<unsigned>(PropertyAttribute::DontEnum), offset);
        RELEASE_ASSERT(offset == static_cast<PropertyOffset>(i));
    }
    return structure;
}

Structure* PyInstance::createStructure(VM& vm, JSGlobalObject* globalObject, Runtime& rt)
{
    return createStructureOfInstances(vm, globalObject, rt, inlineCapacity);
}

PyInstance* PyInstance::create(VM& vm, PyType* type)
{
    Structure* structure = type->instanceStructure(vm); // It may have to be made: not between the allocation of the cell and its initialization.
    auto* object = new (NotNull, allocateCell<PyInstance>(vm, allocationSize(structure->inlineCapacity()))) PyInstance(vm, structure, type);
    // Not that of JSNonFinalObject, which takes it that no such object has storage of its own for properties.
    object->JSObjectWithButterfly::finishCreation(vm);
    return object;
}

// The key of the property that holds the attribute. Null: there can be no such property.
static ALWAYS_INLINE UniquedStringImpl* existingAttributeKey(JSString* name, RefPtr<AtomStringImpl>& holder)
{
    StringImpl* impl = name->tryGetValueImpl();
    ASSERT(impl);
    if (!impl->isAtom()) [[unlikely]] {
        holder = AtomStringImpl::lookUp(impl);
        impl = holder.get();
        if (!impl)
            return nullptr;
    }
    auto* key = static_cast<UniquedStringImpl*>(impl);
    if (parseIndex(*key)) [[unlikely]]
        return nullptr;
    return key;
}

JSValue PyInstance::getAttribute(VM& vm, JSString* name)
{
    if (PyDict* dict = m_dict.get()) [[unlikely]]
        return dict->get(name);
    RefPtr<AtomStringImpl> holder;
    UniquedStringImpl* key = existingAttributeKey(name, holder);
    if (!key) [[unlikely]]
        return { };
    return getDirect(vm, PropertyName(key));
}

void PyInstance::setAttribute(JSGlobalObject* globalObject, JSString* name, JSValue value)
{
    VM& vm = getVM(globalObject);
    if (PyDict* dict = m_dict.get()) [[unlikely]]
        return dict->set(vm, name, value);
    Identifier key = Identifier::fromString(vm, name->tryGetValue());
    if (parseIndex(key)) [[unlikely]]
        return ensureDict(globalObject)->set(vm, name, value);
    // What JS gets to see of the attributes is for JSView to say, which then lets JSObject add what it knows of.
    putDirect(vm, key, value, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

bool PyInstance::deleteAttribute(JSGlobalObject* globalObject, JSString* name)
{
    VM& vm = getVM(globalObject);
    if (PyDict* dict = m_dict.get()) [[unlikely]]
        return dict->remove(vm, name);
    RefPtr<AtomStringImpl> holder;
    UniquedStringImpl* key = existingAttributeKey(name, holder);
    if (!key || !getDirect(vm, PropertyName(key)))
        return false;
    DeletePropertySlot slot;
    JSObject::deleteProperty(this, globalObject, PropertyName(key), slot);
    return true;
}

bool PyInstance::hasAttributes(VM& vm)
{
    if (PyDict* dict = m_dict.get())
        return !dict->isEmpty();
    bool found = false;
    structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        found = !entry.key()->isSymbol();
        return !found;
    });
    return found;
}

void PyInstance::forEachAttribute(JSGlobalObject* globalObject, const ScopedLambda<bool(JSString*, JSValue)>& function)
{
    VM& vm = getVM(globalObject);
    if (PyDict* dict = m_dict.get()) {
        JSValue key, value;
        for (unsigned position = 0; dict->next(position, key, value);) {
            if (key.isString() && !function(asString(key), value))
                return;
        }
        return;
    }
    // The function may do anything to the object.
    Vector<std::pair<RefPtr<UniquedStringImpl>, PropertyOffset>, 8> properties;
    structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (!entry.key()->isSymbol())
            properties.append({ entry.key(), entry.offset() });
        return true;
    });
    MarkedArgumentBuffer values;
    for (auto& property : properties)
        values.append(getDirect(property.second));
    for (unsigned i = 0; i < properties.size(); ++i) {
        // One cell for a name, however many objects have an attribute of that name.
        JSString* name;
        {
            DeferGC deferGC(vm);
            name = vm.atomStringToJSStringMap.ensureValue(properties[i].first.get(), [&] {
                return jsString(vm, String(properties[i].first.get()));
            });
        }
        if (!function(name, values.at(i)))
            return;
    }
}

PyDict* PyInstance::ensureDict(JSGlobalObject* globalObject)
{
    if (PyDict* dict = m_dict.get())
        return dict;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    Structure* before = structure();
    unsigned otherProperties = 0;
    unsigned attributeCount = 0;
    before->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        ++(entry.key()->isSymbol() ? otherProperties : attributeCount);
        return true;
    });
    PyDict* dict = PyDict::createWithCapacity(globalObject, attributeCount);
    Vector<RefPtr<UniquedStringImpl>, 8> keys;
    forEachAttribute(globalObject, [&](JSString* name, JSValue value) {
        dict->set(vm, name, value);
        keys.append(static_cast<UniquedStringImpl*>(name->tryGetValueImpl()));
        return true;
    });
    ASSERT(structure() == before);
    // No instruction that has seen an object without some attribute, by its Structure, takes this one for such: the Structure has a
    // property that says so.
    constexpr unsigned attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);
    PropertyOffset offset;
    Structure* after = Structure::addPropertyTransition(vm, type()->instanceStructure(vm), rt.instanceHasDictKey(), attributes, offset);
    if (otherProperties == numberOfFields && !before->isDictionary() && !before->hasIndexingHeader(this) && before->inlineCapacity() == after->inlineCapacity() && isInlineOffset(offset)) [[likely]] {
        // There is nothing to the object but its attributes. It goes straight to the one Structure that all such instances of the
        // class have, and lets go of the storage beside it.
        before->didTransitionFromThisStructureWithoutFiringWatchpoint();
        before->fireStructureTransitionWatchpoint(nullptr);
        nukeStructureAndSetButterfly(vm, before->id(), nullptr);
        for (unsigned i = numberOfFields; i < before->inlineCapacity(); ++i)
            inlineStorage()[i].clear();
        putDirectOffset(vm, offset, jsBoolean(true));
        setStructure(vm, after);
    } else {
        // What __slots__ names stays where it is.
        for (auto& key : keys) {
            DeletePropertySlot slot;
            JSObject::deleteProperty(this, globalObject, PropertyName(key.get()), slot);
        }
        putDirect(vm, rt.instanceHasDictKey(), jsBoolean(true), attributes);
    }
    m_dict.set(vm, this, dict);
    return dict;
}

void PyInstance::setDict(JSGlobalObject* globalObject, PyDict* dict)
{
    ensureDict(globalObject);
    m_dict.set(getVM(globalObject), this, dict);
}

void PyInstance::replaceDict(VM& vm, PyDict* dict)
{
    m_dict.setMayBeNull(vm, this, dict);
}

template<typename Visitor>
void PyInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    // As for a JSFinalObject. The fields are the first of the properties.
    auto* thisObject = uncheckedDowncast<PyInstance>(cell);
    JSCell::visitChildren(thisObject, visitor);
    if (Structure* structure = thisObject->visitButterfly(visitor)) {
        if (unsigned storageSize = structure->inlineSize())
            visitor.appendValuesHidden(thisObject->inlineStorage(), storageSize);
    }
}

DEFINE_VISIT_CHILDREN(PyInstance);

// ---- PyRecord ----

PY_DEFINE_CELL(PyRecord);

PyRecord::PyRecord(VM& vm, Structure* structure, PyType* type, unsigned fieldCount)
    : Base(vm, structure, type)
    , m_fieldCount(fieldCount)
{
    for (unsigned i = 0; i < fieldCount; ++i)
        fields()[i].clear();
}

PyRecord* PyRecord::create(VM& vm, Structure* structure, PyType* type, unsigned fieldCount)
{
    auto* record = new (NotNull, allocateCell<PyRecord>(vm, allocationSize(fieldCount))) PyRecord(vm, structure, type, fieldCount);
    record->finishCreation(vm);
    return record;
}

PyRecord* PyRecord::create(JSGlobalObject* globalObject, PyType* type, unsigned fieldCount)
{
    Runtime& rt = runtime(globalObject);
    return create(rt.vm(), rt.structure<PyRecord>(), type, fieldCount);
}

PyRecord* PyRecord::tryFrom(JSValue value, PyType* type)
{
    if (!is(value))
        return nullptr;
    auto* record = uncheckedDowncast<PyRecord>(value.asCell());
    return record->type() == type || record->type()->isSubtypeOf(type) ? record : nullptr;
}

template<typename Visitor>
void PyRecord::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyRecord>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.appendValues(thisObject->fields(), thisObject->m_fieldCount);
}

DEFINE_VISIT_CHILDREN(PyRecord);

// ---- PyTuple ----

PY_DEFINE_CELL(PyTuple);

PyTuple::PyTuple(VM& vm, Structure* structure, PyType* type, unsigned length)
    : Base(vm, structure, type)
    , m_length(length)
{
    for (unsigned i = 0; i < length; ++i)
        items()[i].setStartingValue(jsUndefined());
}

PyTuple::PyTuple(VM& vm, Structure* structure, PyType* type, unsigned length, const JSValue* first, ptrdiff_t step)
    : Base(vm, structure, type)
    , m_length(length)
{
    for (unsigned i = 0; i < length; ++i, first += step)
        items()[i].setStartingValue(*first);
}

PyTuple* PyTuple::createUninitialized(VM& vm, Structure* structure, PyType* type, unsigned length)
{
    auto* tuple = new (NotNull, allocateCell<PyTuple>(vm, allocationSize(length))) PyTuple(vm, structure, type, length);
    tuple->finishCreation(vm);
    return tuple;
}

PyTuple* PyTuple::create(JSGlobalObject* globalObject, unsigned length)
{
    Runtime& rt = runtime(globalObject);
    if (!length)
        return rt.emptyTuple();
    return createUninitialized(rt.vm(), rt.structure<PyTuple>(), rt.tupleType(), length);
}

PyTuple* PyTuple::create(JSGlobalObject* globalObject, const JSValue* first, ptrdiff_t step, unsigned length)
{
    Runtime& rt = runtime(globalObject);
    if (!length)
        return rt.emptyTuple();
    VM& vm = rt.vm();
    auto* tuple = new (NotNull, allocateCell<PyTuple>(vm, allocationSize(length))) PyTuple(vm, rt.structure<PyTuple>(), rt.tupleType(), length, first, step);
    tuple->finishCreation(vm);
    return tuple;
}

PyTuple* PyTuple::create(JSGlobalObject* globalObject, std::span<const JSValue> values)
{
    return create(globalObject, values.data(), 1, static_cast<unsigned>(values.size()));
}

PyTuple* PyTuple::createFromNegativeIndexed(JSGlobalObject* globalObject, const JSValue* first, unsigned length)
{
    return create(globalObject, first, -1, length);
}

PyTuple* PyTuple::empty(JSGlobalObject* globalObject)
{
    return runtime(globalObject).emptyTuple();
}

template<typename Visitor>
void PyTuple::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyTuple>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.appendValues(thisObject->items(), thisObject->m_length);
}

DEFINE_VISIT_CHILDREN(PyTuple);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
