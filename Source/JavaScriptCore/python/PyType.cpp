/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PyType.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyObjects.h"
#include "PythonOperations.h"
#include "PythonSignatures.h"

namespace JSC {

const ClassInfo PyType::s_info = { "type"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyType) };

void PyType::destroy(JSCell* cell)
{
    static_cast<PyType*>(cell)->PyType::~PyType();
}

template<typename Visitor>
void PyType::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyType>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_metatype);
    visitor.append(thisObject->m_base);
    visitor.append(thisObject->m_bases);
    visitor.append(thisObject->m_mro);
    visitor.append(thisObject->m_instanceStructure);
    visitor.append(thisObject->m_javaScriptConstructor);
    visitor.append(thisObject->m_javaScriptPrototype);
}

DEFINE_VISIT_CHILDREN(PyType);

Structure* PyType::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(PyTypeType, StructureFlags | pythonCellFlags), info());
}

bool PyType::instancesAreWhatLiteralsMake(JSValue prototype)
{
    auto* type = dynamicDowncast<PyType>(prototype);
    return type && type->instancesAreWhatLiteralsMake();
}

Structure* PyType::createInstanceStructure(VM& vm, JSGlobalObject* globalObject, Layout layout, JSObject* prototype, unsigned additionalFlags)
{
    switch (layout) {
    case Layout::Object:
        return PyInstance::createStructure(vm, globalObject, prototype);
    case Layout::Tuple:
        return PyTuple::createStructure(vm, globalObject, prototype, additionalFlags);
    case Layout::Dict:
        return PyDict::createStructure(vm, globalObject, prototype, additionalFlags);
    case Layout::Set:
        return PySet::createStructure(vm, globalObject, prototype, additionalFlags);
    case Layout::List:
        return PyDerivedList::createStructure(vm, globalObject, prototype);
    case Layout::Exception:
        return PyException::createStructure(vm, globalObject, prototype, additionalFlags);
    case Layout::Boxed:
        return PyBoxedValue::createStructure(vm, globalObject, prototype);
    case Layout::Type:
    case Layout::Native:
    case Layout::JavaScript:
        return nullptr;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PyType* PyType::createBuiltin(VM& vm, JSGlobalObject* globalObject, ASCIILiteral name, PyType* base, Layout layout, unsigned flags)
{
    JSValue prototype = base ? JSValue(base) : JSValue(globalObject->objectPrototype());
    // Before there is a cell for it: making a Structure can set off a collection, which is not to come upon a cell that nothing has been put in.
    Structure* structure = createStructure(vm, globalObject, prototype);
    auto* type = new (NotNull, allocateCell<PyType>(vm)) PyType(vm, structure);
    type->finishCreation(vm);
    if (size_t dot = reverseFind(name.span8(), '.'); dot != notFound) {
        type->m_dottedName = name;
        name = ASCIILiteral::fromLiteralUnsafe(name.characters() + dot + 1);
    }
    type->m_originalName.set(vm, type, jsString(vm, String(name)));
    type->m_base.setMayBeNull(vm, type, base);
    type->m_layout = layout;
    type->m_flags = flags;
    // One that is not CPython's is as its base is.
    if (auto* cpython = Python::findTypeLayout(name)) {
        type->m_basicSize = cpython->basicSize;
        type->m_itemSize = cpython->itemSize;
        type->m_dictOffset = cpython->dictOffset;
        type->m_weakReferenceOffset = cpython->weakReferenceOffset;
        type->m_flagsForPython = cpython->flags;
        type->m_isCPythons = true;
    } else if (base) {
        type->m_flagsForPython = base->m_flagsForPython;
        type->m_basicSize = base->m_basicSize;
        type->m_itemSize = base->m_itemSize;
        type->m_dictOffset = base->m_dictOffset;
        type->m_weakReferenceOffset = base->m_weakReferenceOffset;
    }
    // What JavaScript makes is laid out in a way of its own, so that a class cannot be derived from it and from dict, say.
    if (layout == Layout::JavaScript)
        type->m_basicSize += sizeof(void*);
    type->m_flags |= (type->m_dictOffset ? HasInstanceDict : 0) | (type->m_weakReferenceOffset ? HasWeakReferences : 0);
    return type;
}

// The bits of type.__flags__: Py_TPFLAGS_* of CPython's Include/object.h
static constexpr unsigned long cpythonInlineValues = 1ul << 2;
static constexpr unsigned long cpythonManagedWeakReferences = 1ul << 3;
static constexpr unsigned long cpythonManagedDict = 1ul << 4;
static constexpr unsigned long cpythonHeapType = 1ul << 9;
static constexpr unsigned long cpythonBaseType = 1ul << 10;
static constexpr unsigned long cpythonHaveVectorcall = 1ul << 11;
static constexpr unsigned long cpythonReady = 1ul << 12;
static constexpr unsigned long cpythonHaveGC = 1ul << 14;
static constexpr unsigned long cpythonIsAbstract = 1ul << 20;
static constexpr unsigned long cpythonMatchSelf = 1ul << 22;
static constexpr unsigned long cpythonItemsAtEnd = 1ul << 23;
static constexpr unsigned long cpythonSubclassOfBuiltin = 0xFFul << 24; // One for each of int, list, tuple, bytes, str, dict, BaseException and type.

void PyType::addToLayout(unsigned slots, bool addsDict, bool addsWeakReferences)
{
    m_basicSize += slots * sizeof(void*);
    m_flagsForPython = cpythonHeapType | cpythonBaseType | cpythonReady | cpythonHaveGC
        | (m_base->m_flagsForPython & (cpythonInlineValues | cpythonManagedWeakReferences | cpythonManagedDict | cpythonHaveVectorcall | cpythonMatchSelf | cpythonItemsAtEnd | cpythonSubclassOfBuiltin))
        | (hasFlag(IsSequence) ? cpythonSequence : 0) | (hasFlag(IsMapping) ? cpythonMapping : 0);
    // Where CPython keeps what it manages itself.
    if (addsDict) {
        m_dictOffset = -1;
        m_flags |= HasInstanceDict;
        m_flagsForPython |= cpythonManagedDict | (m_itemSize ? 0 : cpythonInlineValues);
    }
    if (addsWeakReferences) {
        m_weakReferenceOffset = -32; // MANAGED_WEAKREF_OFFSET
        m_flags |= HasWeakReferences;
        m_flagsForPython |= cpythonManagedWeakReferences;
    }
}

unsigned long PyType::flagsForPython() const
{
    // What kind of pattern matches it can be changed at any time, by registering it with collections.abc.Sequence or collections.abc.Mapping.
    return (m_flagsForPython & ~(cpythonSequence | cpythonMapping)) | (hasFlag(IsSequence) ? cpythonSequence : 0) | (hasFlag(IsMapping) ? cpythonMapping : 0) | (hasFlag(IsAbstract) ? cpythonIsAbstract : 0);
}

void PyType::finishBuiltin(VM& vm, JSGlobalObject* globalObject, PyType* metatype)
{
    m_metatype.set(vm, this, metatype);
    PyType* base = m_base.get();
    unsigned depth = 1;
    for (PyType* type = base; type; type = type->base())
        ++depth;
    PyTuple* mro = PyTuple::create(globalObject, depth);
    unsigned i = 0;
    for (PyType* type = this; type; type = type->base())
        mro->initializeAt(vm, i++, type);
    m_mro.set(vm, this, mro);
    m_bases.set(vm, this, base ? PyTuple::create(globalObject, { base }) : PyTuple::create(globalObject, 0));
    // int's, float's, str's and list's own instances are JavaScript's values.
    if (m_layout != Layout::Boxed && m_layout != Layout::List)
        m_instanceStructure.setMayBeNull(vm, this, createInstanceStructure(vm, globalObject, m_layout, this));
    if (base)
        base->addSubclass(this);
}

Structure* PyType::createInstanceStructure(VM& vm, JSGlobalObject* globalObject, PyType* base, JSObject* prototype)
{
    // The class is a program's, so it can have __call__(), now or later. A tuple or a dict of no such class never can, and nothing is to be asked of one to find that out.
    if (Structure* structure = createInstanceStructure(vm, globalObject, base->layout(), prototype, OverridesGetCallData))
        return structure;
    // The same kind of cell as the base's instances, whatever that is.
    if (base->layout() != Layout::Native || !base->instanceStructure())
        return nullptr;
    if (base->instanceStructure()->typeInfo().type() == Uint8ArrayType)
        return PyDerivedBytes::createStructure(vm, globalObject, prototype);
    TypeInfo typeInfo = base->instanceStructure()->typeInfo();
    static_assert(OverridesGetCallData <= std::numeric_limits<TypeInfo::InlineTypeFlags>::max());
    return Structure::create(vm, globalObject, prototype, TypeInfo(typeInfo.type(), typeInfo.inlineTypeFlags() | OverridesGetCallData, typeInfo.outOfLineTypeFlags()), base->instanceStructure()->classInfoForCells());
}

PyType* PyType::create(VM& vm, JSGlobalObject* globalObject, PyType* metatype, JSString* name, PyTuple* bases, PyType* base, PyTuple* mro)
{
    // Before there is a cell for it: making a Structure can set off a collection, which is not to come upon a cell that nothing has been put in.
    Structure* structure = createStructure(vm, globalObject, (bases->length() ? asType(bases->at(0)) : base)->prototypeObject());
    auto* type = new (NotNull, allocateCell<PyType>(vm)) PyType(vm, structure);
    type->finishCreation(vm);
    type->m_metatype.set(vm, type, metatype);
    type->m_originalName.set(vm, type, name);
    type->m_base.set(vm, type, base);
    type->m_bases.set(vm, type, bases);
    type->m_layout = base->layout();
    type->m_errorType = base->m_errorType;
    type->m_flags = IsHeapType | IsBaseType | (base->m_flags & (IsExceptionType | IsTypeSubclass | MatchesSelf | IsBytes | HasInstanceDict | HasWeakReferences | NewIsLookedFor));
    type->m_basicSize = base->m_basicSize;
    type->m_itemSize = base->m_itemSize;
    type->m_dictOffset = base->m_dictOffset;
    type->m_weakReferenceOffset = base->m_weakReferenceOffset;
    // Whether it is a sequence or a mapping is for the first of its ancestors that is one or the other to say.
    for (auto& ancestor : mro->span()) {
        if (unsigned collectionFlags = asType(ancestor.get())->m_flags & (IsSequence | IsMapping)) {
            type->m_flags |= collectionFlags;
            break;
        }
    }
    // The class comes first in its own order, and could not be put there before there was one.
    PyTuple* fullOrder = PyTuple::create(globalObject, mro->length() + 1);
    fullOrder->initializeAt(vm, 0, type);
    for (unsigned i = 0; i < mro->length(); ++i)
        fullOrder->initializeAt(vm, i + 1, mro->at(i));
    type->m_mro.set(vm, type, fullOrder);
    type->m_instanceStructure.setMayBeNull(vm, type, createInstanceStructure(vm, globalObject, base, type));
    for (auto& direct : bases->span())
        asType(direct.get())->addSubclass(type);
    // What a built-in class of JavaScript's defines may not be there until it is asked for by name, and lookup() does not ask.
    for (auto& ancestor : mro->span()) {
        for (JSObject* object : { asType(ancestor.get())->javaScriptPrototype(), asType(ancestor.get())->javaScriptConstructor() }) {
            if (object && !object->staticPropertiesReified())
                object->reifyAllStaticProperties(globalObject);
        }
    }
    return type;
}

void PyType::setJavaScriptClass(VM& vm, JSObject* constructor, JSObject* prototype)
{
    m_javaScriptConstructor.set(vm, this, constructor);
    m_javaScriptPrototype.set(vm, this, prototype);
    constructor->putDirect(vm, vm.pythonNames().private_class, this, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
}

PyType* PyType::createForJavaScript(VM& vm, JSGlobalObject* globalObject, JSObject* constructor, JSObject* prototype, PyType* base)
{
    // Before there is a cell for it: making a Structure can set off a collection, which is not to come upon a cell that nothing has been put in.
    Structure* structure = createStructure(vm, globalObject, jsNull());
    auto* type = new (NotNull, allocateCell<PyType>(vm)) PyType(vm, structure);
    type->finishCreation(vm);
    type->m_javaScriptConstructor.set(vm, type, constructor);
    type->m_javaScriptPrototype.set(vm, type, prototype);
    type->m_metatype.set(vm, type, base->metatype());
    type->m_originalName.set(vm, type, jsString(vm, getCalculatedDisplayName(vm, constructor)));
    type->m_base.set(vm, type, base);
    type->m_bases.set(vm, type, PyTuple::create(globalObject, { base->object() }));
    type->m_layout = base->layout();
    type->m_errorType = base->m_errorType;
    type->m_flags = IsHeapType | IsBaseType | (base->m_flags & (IsExceptionType | IsTypeSubclass | MatchesSelf | IsBytes | HasInstanceDict | HasWeakReferences | IsSequence | IsMapping | IsJavaScript));
    type->m_basicSize = base->m_basicSize;
    type->m_itemSize = base->m_itemSize;
    type->m_dictOffset = base->m_dictOffset;
    type->m_weakReferenceOffset = base->m_weakReferenceOffset;
    // A constructor that is not written in JavaScript makes a kind of cell of its own: a Map is not a Set. So a class cannot be derived from both.
    auto* function = dynamicDowncast<JSFunction>(constructor);
    if (type->m_layout == Layout::JavaScript && (!function || function->isHostOrBuiltinFunction()))
        type->m_basicSize += sizeof(void*);
    // There are no __slots__ in JavaScript.
    type->addToLayout(0, !type->hasFlag(HasInstanceDict), !type->hasFlag(HasWeakReferences));
    PyTuple* order = PyTuple::create(globalObject, base->mro()->length() + 1);
    order->initializeAt(vm, 0, type);
    for (unsigned i = 0; i < base->mro()->length(); ++i)
        order->initializeAt(vm, i + 1, base->mro()->at(i));
    type->m_mro.set(vm, type, order);
    type->m_instanceStructure.setMayBeNull(vm, type, createInstanceStructure(vm, globalObject, base, prototype));
    base->addSubclass(type);
    return type;
}

void PyType::setName(VM& vm, JSString* name)
{
    if (!m_dottedName.isNull()) [[unlikely]] {
        auto& module = vm.pythonNames().dunder_module;
        if (!getDirect(vm, module))
            putDirect(vm, module, jsString(vm, moduleOfBuiltin()));
        m_dottedName = { };
    }
    m_originalName.set(vm, this, name);
}

String PyType::nameString(JSGlobalObject* globalObject) const
{
    if (!m_dottedName.isNull()) [[unlikely]]
        return m_dottedName;
    return m_originalName->value(globalObject);
}

String PyType::moduleOfBuiltin() const
{
    if (m_dottedName.isNull())
        return { };
    return String(m_dottedName.span8().first(reverseFind(m_dottedName.span8(), '.')));
}

String PyType::nameWithoutModule(JSGlobalObject* globalObject) const
{
    return m_originalName->value(globalObject);
}

JSValue PyType::lookup(VM& vm, PropertyName name) const
{
    if (Python::isIndexLike(name)) [[unlikely]] {
        for (auto& entry : m_mro->span()) {
            if (JSValue value = Python::getIndexLikeAttribute(vm, asObject(entry.get()), name))
                return value;
        }
        return { };
    }
    for (auto& entry : m_mro->span()) {
        if (JSValue value = asObject(entry.get())->getDirect(vm, name))
            return value;
        // What a class of JavaScript's defines for its instances are properties of their prototype.
        if (JSObject* prototype = asType(entry.get())->javaScriptPrototype(); prototype && name != vm.propertyNames->constructor) [[unlikely]] {
            if (JSValue value = prototype->getDirect(vm, name))
                return value;
        }
    }
    return { };
}

JSValue PyType::lookupWithoutAllocating(UniquedStringImpl* name) const
{
    // JSObject::getDirect() makes the Structure a table of its properties if it has none just now.
    auto get = [&] (JSObject* object) -> JSValue {
        PropertyOffset offset = object->structure()->getConcurrently(name);
        return offset == invalidOffset ? JSValue() : object->getDirect(offset);
    };
    for (auto& entry : m_mro->span()) {
        if (JSValue value = get(asObject(entry.get())))
            return value;
        if (JSObject* prototype = asType(entry.get())->javaScriptPrototype()) [[unlikely]] {
            if (JSValue value = get(prototype))
                return value;
        }
    }
    return { };
}

JSValue PyType::lookup(VM& vm, PropertyName name, PyType*& holder) const
{
    holder = nullptr;
    if (Python::isIndexLike(name)) [[unlikely]]
        return lookup(vm, name);
    for (auto& entry : m_mro->span()) {
        if (JSValue value = asObject(entry.get())->getDirect(vm, name)) {
            holder = asType(entry.get());
            return value;
        }
        if (JSObject* prototype = asType(entry.get())->javaScriptPrototype(); prototype && name != vm.propertyNames->constructor) [[unlikely]] {
            if (JSValue value = prototype->getDirect(vm, name))
                return value;
        }
    }
    return { };
}

JSValue PyType::lookupOnClass(VM& vm, PropertyName name, bool& isStatic, PyType* from) const
{
    isStatic = false;
    for (auto& entry : m_mro->span()) {
        PyType* type = asType(entry.get());
        if (from) {
            if (type != from)
                continue;
            from = nullptr;
        }
        // These say what kind of function the constructor is, and are nothing that the class defines.
        if (JSObject* constructor = type->javaScriptConstructor(); constructor && name != vm.propertyNames->prototype && name != vm.propertyNames->name && name != vm.propertyNames->length) [[unlikely]] {
            if (JSValue value = constructor->getDirect(vm, name)) {
                isStatic = true;
                return value;
            }
        }
        if (JSValue value = type->lookupOwn(vm, name))
            return value;
    }
    return { };
}

JSValue PyType::lookupOwn(VM& vm, PropertyName name) const
{
    if (Python::isIndexLike(name)) [[unlikely]]
        return Python::getIndexLikeAttribute(vm, const_cast<PyType*>(this), name);
    if (JSValue value = getDirect(vm, name))
        return value;
    // `constructor` says which class this is, and is nothing that it defines.
    if (m_javaScriptPrototype && name != vm.propertyNames->constructor) [[unlikely]]
        return m_javaScriptPrototype->getDirect(vm, name);
    return { };
}

JSValue PyType::lookupAfter(VM& vm, PyType* after, PropertyName name) const
{
    auto order = m_mro->span();
    size_t i = 0;
    while (i < order.size() && order[i].get().asCell() != after)
        ++i;
    for (++i; i < order.size(); ++i) {
        if (JSValue value = asType(order[i].get())->lookupOwn(vm, name))
            return value;
    }
    return { };
}

JSValue PyType::lookupFrom(VM& vm, PyType* from, PropertyName name) const
{
    auto order = m_mro->span();
    size_t i = 0;
    while (i < order.size() && order[i].get().asCell() != from)
        ++i;
    for (; i < order.size(); ++i) {
        if (JSValue value = asType(order[i].get())->lookupOwn(vm, name))
            return value;
    }
    return { };
}

bool PyType::isSubtypeOf(const PyType* other) const
{
    if (this == other)
        return true;
    for (auto& entry : m_mro->span()) {
        if (entry.get().asCell() == other)
            return true;
    }
    return false;
}

void PyType::addSubclass(PyType* subclass)
{
    // Those that are gone are taken out when there are twice as many as were left the last time, and not each time. Every class is derived from object, so making one would take as long as there are classes.
    if (m_subclasses.size() >= m_subclassCountToSweepAt) {
        m_subclasses.removeAllMatching([] (auto& weak) { return !weak; });
        m_subclassCountToSweepAt = 2 * m_subclasses.size() + 1;
    }
    m_subclasses.append(Weak<PyType>(subclass));
}

void PyType::removeSubclass(PyType* subclass)
{
    m_subclasses.removeAllMatching([&] (auto& weak) { return !weak || weak.get() == subclass; });
}

void PyType::setBases(VM& vm, PyTuple* bases, PyType* base)
{
    for (auto& old : m_bases->span())
        asType(old.get())->removeSubclass(this);
    m_bases.set(vm, this, bases);
    m_base.set(vm, this, base);
    for (auto& direct : bases->span())
        asType(direct.get())->addSubclass(this);
    // To JavaScript it is derived from the first of them.
    setPrototypeDirect(vm, asType(bases->at(0))->prototypeObject());
}

void PyType::setOrder(VM& vm, PyTuple* order)
{
    m_mro.set(vm, this, order);
    m_flags &= ~(IsSequence | IsMapping);
    for (auto& ancestor : order->span()) {
        if (unsigned collectionFlags = asType(ancestor.get())->m_flags & (IsSequence | IsMapping); collectionFlags && ancestor.get() != this) {
            m_flags |= collectionFlags;
            break;
        }
    }
    // What it finds, and where, may all be different.
    m_knowsHooks = false;
    constructionMayHaveChanged(vm);
    std::exchange(m_instanceAccessIsAsFound, WatchpointSet::create(IsWatched))->fireAll(vm, "The order in which the bases of a class are searched was changed");
}

Vector<PyType*> PyType::subclasses() const
{
    Vector<PyType*> result;
    for (auto& weak : m_subclasses) {
        if (PyType* subclass = weak.get())
            result.append(subclass);
    }
    return result;
}

void PyType::instanceAccessMayHaveChanged(VM& vm)
{
    std::exchange(m_instanceAccessIsAsFound, WatchpointSet::create(IsWatched))->fireAll(vm, "A class was given something that changes how the attributes of its instances are found");
    for (PyType* subclass : subclasses())
        subclass->instanceAccessMayHaveChanged(vm);
}

void PyType::setAttribute(VM& vm, PropertyName name, JSValue value)
{
    auto& names = vm.pythonNames();
    JSValue previous = lookupOwn(vm, name);
    Python::putStoredAttribute(vm, this, name, value);
    attributeDidChange(vm, name);
    if (name == names.dunder_getattribute || name == names.dunder_setattr || name == names.dunder_delattr || Python::isDataDescriptor(globalObject(), value) || (previous && !Python::isGotFromInstanceInTheSameWay(globalObject(), previous, value)))
        instanceAccessMayHaveChanged(vm);
}

bool PyType::deleteAttribute(VM& vm, JSGlobalObject* globalObject, PropertyName name)
{
    if (!lookupOwn(vm, name))
        return false;
    attributeDidChange(vm, name);
    return Python::deleteStoredAttribute(globalObject, this, name);
}

void PyType::attributeDidChange(VM& vm, PropertyName name)
{
    auto& names = vm.pythonNames();
    if (name == names.dunder_getattribute || name == names.dunder_getattr || name == names.dunder_setattr || name == names.dunder_delattr)
        forgetHooks();
    if (name == names.dunder_new)
        updateWhetherNewIsLookedFor(vm);
    if (name == names.dunder_new || name == names.dunder_init)
        constructionMayHaveChanged(vm);
}

void PyType::updateWhetherNewIsLookedFor(VM& vm)
{
    auto& name = vm.pythonNames().dunder_new;
    JSValue found = lookup(vm, name);
    auto* native = found && found.isCell() ? dynamicDowncast<PyNativeFunction>(found.asCell()) : nullptr;
    if (!native || native->kind() != PyNativeFunction::Kind::New)
        m_flags |= NewIsLookedFor;
    // update_subclasses()
    for (PyType* subclass : subclasses()) {
        if (!subclass->lookupOwn(vm, name))
            subclass->updateWhetherNewIsLookedFor(vm);
    }
}

bool PyType::newIsThatOfObject(JSGlobalObject* globalObject) const
{
    if (hasFlag(NewIsLookedFor))
        return false;
    JSValue found = lookup(globalObject->vm(), globalObject->vm().pythonNames().dunder_new);
    return found && found.asCell() == globalObject->pyRealm()->function(PyRealm::WellKnownFunction::ObjectNew);
}

PyType::Construction* PyType::construction(JSGlobalObject* globalObject)
{
    if (m_knowsConstruction) [[likely]]
        return m_construction.get();
    RefPtr<Construction> construction = Python::workOutConstruction(globalObject, this);
    Locker locker { cellLock() };
    m_construction = WTF::move(construction);
    m_knowsConstruction = true;
    return m_construction.get();
}

RefPtr<PyType::Construction> PyType::constructionConcurrently()
{
    Locker locker { cellLock() };
    return m_construction;
}

void PyType::constructionMayHaveChanged(VM& vm)
{
    RefPtr<Construction> old;
    {
        Locker locker { cellLock() };
        old = std::exchange(m_construction, nullptr);
        m_knowsConstruction = false;
    }
    if (old)
        old->isAsFound->fireAll(vm, "What calling a class comes to was changed");
    // What is derived from it may have it from it.
    for (PyType* subclass : subclasses())
        subclass->constructionMayHaveChanged(vm);
}

void PyType::forgetHooks()
{
    m_knowsHooks = false;
    // What is derived from it may have it from it.
    for (PyType* subclass : subclasses())
        subclass->forgetHooks();
}

unsigned PyType::hooks(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    if (m_knowsHooks) [[likely]]
        return m_flags & hookFlags;

    using Function = PyRealm::WellKnownFunction;
    PyRealm* realm = globalObject->pyRealm();
    auto isOneOf = [&] (JSValue value, Function a, Function b) {
        return value && value.isCell() && (value.asCell() == realm->function(a) || value.asCell() == realm->function(b));
    };
    // A class that is written in C++ can have a __getattribute__ of its own that is object's by another name, as one has in CPython if it is made when its module is and says that its tp_getattro is
    // PyObject_GenericGetAttr(). See addGenericGetAttribute().
    auto isObjectsOwn = [&] (JSValue value) {
        auto* function = value && value.isCell() ? dynamicDowncast<JSFunction>(value.asCell()) : nullptr;
        return function && function->isHostFunction() && function->nativeFunction() == uncheckedDowncast<JSFunction>(realm->function(Function::ObjectGetAttribute))->nativeFunction();
    };
    unsigned flags = 0;
    // What module's does besides is done for a module in any case. See getAttribute().
    JSValue getAttribute = lookup(vm, names.dunder_getattribute);
    if (!isOneOf(getAttribute, Function::ObjectGetAttribute, Function::TypeGetAttribute) && !isOneOf(getAttribute, Function::ModuleGetAttribute, Function::ModuleGetAttribute) && !isObjectsOwn(getAttribute))
        flags |= HasCustomGetAttribute;
    if (lookup(vm, names.dunder_getattr))
        flags |= HasGetAttr;
    if (!isOneOf(lookup(vm, names.dunder_setattr), Function::ObjectSetAttr, Function::TypeSetAttr) || !isOneOf(lookup(vm, names.dunder_delattr), Function::ObjectDelAttr, Function::TypeDelAttr))
        flags |= HasCustomSetAttr;
    m_flags = (m_flags & ~hookFlags) | flags;
    m_knowsHooks = true;
    return flags;
}

bool PyType::cannotBeInstantiated(VM& vm) const
{
    // CPython says so of those that it has. One that it does not have is like those of its own that have no __new__ and are derived from nothing that has.
    constexpr unsigned long disallowsInstantiation = 1ul << 7;
    if (m_flagsForPython & disallowsInstantiation)
        return true;
    return !m_isCPythons && !hasFlag(IsHeapType) && base() && !base()->base() && !getDirect(vm, vm.pythonNames().dunder_new);
}

static JSC_DECLARE_HOST_FUNCTION(callType);

// C(...) is type(C).__call__(C, ...), which for nearly every class is type.__call__.
JSValue PyType::call(JSGlobalObject* globalObject, const ArgList& arguments, Python::KeywordNames* keywordNames)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = this;
    // What BaseException makes of what is given by name is for it to say.
    if (Construction* construction = this->construction(globalObject); construction && !(keywordNames && construction->kind == Construction::Kind::Exception))
        RELEASE_AND_RETURN(scope, Python::construct(globalObject, type, *construction, arguments, keywordNames));
    if (m_vectorcall && !keywordNames) {
        JSValue result = m_vectorcall(globalObject, arguments);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }

    PyType* metatype = type->metatype();
    if (metatype != realm->typeType()) {
        JSValue function = metatype->lookup(vm, vm.pythonNames().dunder_call);
        if (function && function.asCell() != realm->function(PyRealm::WellKnownFunction::TypeCall)) {
            JSValue bound = Python::bindDescriptor(globalObject, function, type, metatype);
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, Python::callWithKeywords(globalObject, bound, arguments, keywordNames));
        }
    }

    RELEASE_AND_RETURN(scope, Python::instantiate(globalObject, type, arguments, keywordNames));
}

JSC_DEFINE_HOST_FUNCTION(callType, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    Python::NativeArguments given(callFrame);
    return JSValue::encode(uncheckedDowncast<PyType>(callFrame->jsCallee())->call(globalObject, given.allFrom(0), given.keywordNames()));
}

static JSC_DECLARE_HOST_FUNCTION(constructType);

// new C(...) is C(...). But it may be for a class derived from C that an instance is wanted: that is so of super(...) in the constructor of a class of
// JavaScript's, and can be asked for with Reflect.construct(). Then this is the part that C and what it is derived from have in making one.
JSC_DEFINE_HOST_FUNCTION(constructType, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto* type = uncheckedDowncast<PyType>(callFrame->jsCallee());
    JSValue newTarget = callFrame->newTarget();
    if (newTarget == JSValue(type) || !isClass(newTarget))
        return callType(globalObject, callFrame);
    PyType* wanted = asType(newTarget);
    if (!wanted->isSubtypeOf(type))
        return callType(globalObject, callFrame);
    return JSValue::encode(Python::instantiateFrom(globalObject, wanted, type, ArgList(callFrame), nullptr));
}

bool PyType::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* type = uncheckedDowncast<PyType>(object);
    JSValue receiver = slot.thisValue();

    // C.prototype is C, since a class is the prototype of its instances. That is JavaScript's way of putting it, and no attribute.
    if (name == vm.propertyNames->prototype) {
        slot.setValue(object, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly, type);
        return true;
    }
    // What it has of its own is what it has. So is what the engine asks about for its own purposes, which is not to run anything.
    bool isGetOrHas = slot.internalMethodType() == PropertySlot::InternalMethodType::Get || slot.internalMethodType() == PropertySlot::InternalMethodType::HasProperty;
    if (!isGetOrHas || name.isPrivateName() || !receiver.isObject()) {
        bool found = Base::getOwnPropertySlot(object, globalObject, name, slot);
        RETURN_IF_EXCEPTION(scope, false);
        // As with what is defined in a class of JavaScript's, it is not gone through by `for (name in instance)`.
        if (found && !slot.isVMInquiry() && slot.isValue()) {
            unsigned attributes = slot.attributes() | PropertyAttribute::DontEnum;
            JSValue value = slot.getPureResult();
            if (slot.isCacheableValue())
                slot.setValue(object, attributes, value, slot.cachedOffset());
            else
                slot.setValue(object, attributes, value);
        }
        return found;
    }

    // In Python, what an attribute is is settled when it is got: a function of the class becomes a method bound to the instance, a property is
    // computed, __getattr__ is asked. So the class of the receiver answers with what getattr() would give. The classes beyond it have
    // nothing to add, since that has been through all of them, in Python's order and not in that of the prototypes.
    //
    // Something of JavaScript's can inherit from a class too, as what Object.create(C) makes does. It is no instance, so what it inherits is what the class
    // itself has: C.name.
    //
    // And a class of JavaScript's can be derived from it. Its instances are instances of this one, and its constructor inherits from this one as a
    // class does from its base.
    if (receiver != JSValue(type)) {
        // The first class that is come to answers for all of them. If another is come to, either that one found nothing, and neither will this, or
        // this is where looking began: super.name, in a class of JavaScript's that is derived from this one. Then it is what this one and those
        // after it define that is wanted, and nothing else.
        JSValue first = asObject(receiver)->getPrototypeDirect();
        while (first.isObject() && !isType(first))
            first = asObject(first)->getPrototypeDirect();
        if (first != JSValue(type)) {
            PyType* receiverType = isClass(receiver) ? nullptr : Python::typeOf(globalObject, receiver);
            if (!receiverType || !receiverType->isSubtypeOf(type))
                return false;
            JSValue attribute = receiverType->lookupFrom(vm, type, name);
            if (!attribute)
                return false;
            JSValue value = Python::bindDescriptor(globalObject, attribute, receiver, receiverType);
            RETURN_IF_EXCEPTION(scope, false);
            slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum), value);
            return true;
        }
        bool inherits = isClass(receiver) ? asType(receiver)->isSubtypeOf(type) : Python::typeOf(globalObject, receiver)->isSubtypeOf(type);
        if (!inherits)
            receiver = type;
    }
    JSValue value = Python::getPropertyForJavaScript(globalObject, receiver, name, type);
    RETURN_IF_EXCEPTION(scope, false);
    if (!value) {
        // To JavaScript a class is a function, and has what functions have. It cannot inherit that, having only the one prototype, which is for
        // its instances to inherit from.
        if (isClass(receiver))
            RELEASE_AND_RETURN(scope, globalObject->functionPrototype()->getOwnPropertySlot(globalObject->functionPrototype(), globalObject, name, slot));
        return false;
    }
    slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum), value);
    return true;
}

void PyType::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    if (mode == DontEnumPropertiesMode::Include)
        object->getOwnNonIndexPropertyNames(globalObject, names, mode);
}

bool PyType::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    // This is come to for the class itself, and for anything that has the class for a prototype and has no such property of its own yet.
    JSValue receiver = slot.thisValue();
    if (name.isSymbol() || !receiver.isObject())
        return Base::put(cell, globalObject, name, value, slot);
    return Python::setPropertyFromJavaScript(globalObject, receiver, name, value, slot);
}

bool PyType::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    if (name.isSymbol())
        return Base::deleteProperty(cell, globalObject, name, slot);
    return Python::deletePropertyFromJavaScript(globalObject, cell, name);
}

bool PyType::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    return Python::definePropertyFromJavaScript(globalObject, object, name, descriptor, shouldThrow);
}

bool PyType::preventExtensions(JSObject*, JSGlobalObject*)
{
    // Whether attributes can be set is for Python to say, one at a time.
    return false;
}

bool PyType::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    return Python::isInstanceOf(globalObject, value, object);
}

PyType::PyType(VM& vm, Structure* structure)
    : Base(vm, structure, callType, constructType)
    , m_instanceAccessIsAsFound(WatchpointSet::create(IsWatched))
{
}

} // namespace JSC
