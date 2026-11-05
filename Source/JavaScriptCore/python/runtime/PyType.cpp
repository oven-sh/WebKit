/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyType.h"

#include "FrameTracers.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDescriptors.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyJSClass.h"
#include "PyJSView.h"
#include "PyList.h"
#include "PyRuntime.h"
#include "PyStr.h"
#include "PyWeakRef.h"
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {


const ClassInfo PyType::s_info = { "PyType"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyType) };

JSC_DEFINE_HOST_FUNCTION(callType, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    CallArgs args(callFrame);
    if (!args.hasKeywords() && hasKeywordMarker(globalObject, callFrame)) [[unlikely]]
        return JSValue::encode(resultForCaller(globalObject, callFrame, callWithKeywordMarker(globalObject, callFrame)));
    // What call() comes to for a class whose class is type.
    auto* type = uncheckedDowncast<PyType>(callFrame->jsCallee());
    if (type->type() == runtime(globalObject).typeType() && getVM(globalObject).isSafeToRecurseSoft()) [[likely]]
        return JSValue::encode(resultForCaller(globalObject, callFrame, typeCall(globalObject, type, args)));
    return JSValue::encode(resultForCaller(globalObject, callFrame, call(globalObject, type, args)));
}

// `new T(...)` in JS.
JSC_DEFINE_HOST_FUNCTION(constructType, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* callee = callFrame->jsCallee();
    // super(...) in the constructor of a class derived from this one.
    PyType* derived = callFrame->newTarget() != JSValue(callee) ? PyType::tryFrom(callFrame->newTarget()) : nullptr;
    if (derived && !derived->isSubtypeOf(uncheckedDowncast<PyType>(callee)))
        derived = nullptr;
    return JSValue::encode(valueForJS(withArgumentsFromJS(globalObject, callFrame, [&](const CallArgs& args) {
        if (derived) [[unlikely]]
            return constructBaseOfDerivedClass(globalObject, uncheckedDowncast<PyType>(callee), derived, args);
        return call(globalObject, callee, args);
    })));
}

PY_DEFINE_JS_VIEW(PyType)

bool PyType::customHasInstance(JSObject* object, JSGlobalObject*, JSValue value)
{
    return isInstance(object->realm(), value, object);
}

PyType::PyType(VM& vm, Structure* structure)
    : Base(vm, structure, callType, constructType)
{
}

PyType* PyType::createBare(VM& vm, Structure* structure)
{
    auto* type = new (NotNull, allocateCell<PyType>(vm)) PyType(vm, structure);
    type->finishCreation(vm);
    type->m_originalName.set(vm, type, jsEmptyString(vm));
    return type;
}

template<typename Visitor>
void PyType::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyType>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_type);
    visitor.append(thisObject->m_name);
    visitor.append(thisObject->m_qualname);
    visitor.append(thisObject->m_module);
    visitor.append(thisObject->m_slots);
    visitor.append(thisObject->m_base);
    visitor.append(thisObject->m_solidBase);
    visitor.append(thisObject->m_bases);
    visitor.append(thisObject->m_mro);
    visitor.append(thisObject->m_dict);
    visitor.append(thisObject->m_subclasses);
    visitor.append(thisObject->m_instanceStructure);
    visitor.append(thisObject->m_jsPrototype);
    visitor.appendValues(thisObject->m_specials.data(), numberOfSpecials);
    for (auto& entry : thisObject->m_lookupCache) {
        visitor.append(entry.name);
        visitor.append(entry.value);
    }
}

DEFINE_VISIT_CHILDREN(PyType);

String PyType::name() const
{
    return stringOf(m_name.get());
}

String PyType::nameInMessages() const
{
    if (isHeapType() || !m_module)
        return name();
    String module = stringOf(m_module.get());
    if (module == "builtins"_s)
        return name();
    return makeString(module, '.', name());
}

bool hasDocstring(ASCIILiteral owner, ASCIILiteral member); // PyDocstrings.cpp

void PyType::initializeBuiltin(JSGlobalObject* globalObject, const TypeDef& def, PyType* metatype, PyType* base)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // Nothing here raises: nothing of the program's runs, and what is allocated is small. The type may be wanted for the first time
    // while an exception propagates, which is set aside.
    SuspendExceptionScope suspended(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    String fullName = def.name;
    size_t dot = fullName.reverseFind('.');
    m_type.set(vm, this, metatype);
    setNameString(vm, internString(vm, dot == notFound ? fullName : fullName.substring(dot + 1)));
    m_flags = def.flags;
    m_dict.set(vm, this, PyDict::create(globalObject));
    if (base) {
        m_base.set(vm, this, base);
        m_flags.add(base->m_flags & inheritedTypeFlags);
        m_bases.set(vm, this, PyTuple::create(globalObject, { JSValue(base) }));
        PyTuple* baseMRO = base->mro();
        ASSERT(baseMRO);
        PyTuple* mro = PyTuple::create(globalObject, baseMRO->length() + 1);
        mro->initializeAt(vm, 0, this);
        for (unsigned i = 0; i < baseMRO->length(); ++i)
            mro->initializeAt(vm, i + 1, baseMRO->at(i));
        m_mro.set(vm, this, mro);
        m_solidBase.set(vm, this, has(TypeFlag::OwnLayout) ? this : base->solidBase());
        base->addSubclass(globalObject, this);
        scope.assertNoException();
    } else {
        m_bases.set(vm, this, rt.emptyTuple());
        m_mro.set(vm, this, PyTuple::create(globalObject, { JSValue(this) }));
        m_solidBase.set(vm, this, this);
    }

    m_module.set(vm, this, dot == notFound ? rt.names.builtins() : internString(vm, fullName.left(dot)));
    JSValue doc = docstringFor(globalObject, makeString("type "_s, def.name));
    m_dict->set(vm, rt.names.dDoc(), isNone(doc) && !def.doc.isNull() ? JSValue(makeStr(vm, def.doc)) : doc);
    addMethods(globalObject, def.methods);
    addGetSets(globalObject, def.getsets);
    if (def.flags.contains(TypeFlag::HasDict) && !(base && base->has(TypeFlag::HasDict)) && !m_dict->get(rt.names.dDict()))
        addGetSets(globalObject, dictGetSet());
    // What CPython makes at run time, much as a class statement does, has what such a class has. Most types outside builtins are
    // made so, and the table of docstrings says which: deque().__module__ is "collections".
    if (def.flags.contains(TypeFlag::Mutable) || hasDocstring(def.name, "__module__"_s))
        m_dict->set(vm, rt.names.dModule(), m_module.get());
    if (def.flags.contains(TypeFlag::Mutable) && def.flags.contains(TypeFlag::HasWeakref) && !(base && base->has(TypeFlag::HasWeakref)))
        addGetSets(globalObject, weakrefGetSet());

    // A type of CPython's that names a function of object's as its own (PyObject_GenericGetAttr, say) has a wrapper of its own
    // for it. Which types do is in the table of docstrings.
    if (base) {
        PyDict* ofObject = rt.objectType()->dict();
        for (auto& slot : repeatableSlotsOfObject()) {
            JSString* name = rt.nameIn(slot);
            if (lookup(name) == ofObject->get(name) && hasDocstring(def.name, slot.name))
                addMethods(globalObject, { &slot, 1 });
        }
    }

    // Nothing makes an instance of a built-in type that has not said how.
    bool hasNew = false;
    for (auto& method : def.methods)
        hasNew |= method.kind == NativeKind::New;
    if (!hasNew && base && (base == rt.objectType() || base->has(TypeFlag::DisallowInstantiation)))
        m_flags.add(TypeFlag::DisallowInstantiation);
}

void PyType::addMethods(JSGlobalObject* globalObject, std::span<const NativeDef> methods)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto add = [&](const NativeDef& def) {
        JSString* name = rt.nameIn(def);
        JSValue value;
        switch (def.kind) {
        case NativeKind::Method:
        case NativeKind::SlotWrapper:
        case NativeKind::ClassMethod:
        case NativeKind::Function:
            value = PyNativeFunction::create(globalObject, def, def.kind, { }, this, name);
            break;
        case NativeKind::New:
            value = PyNativeFunction::create(globalObject, def, NativeKind::New, this, this, name);
            break;
        case NativeKind::StaticMethod: {
            auto* wrapper = PyWrapperObject::create(globalObject, rt.staticmethodType(), PyNativeFunction::create(globalObject, def, NativeKind::Function, { }, this, name));
            wrapper->setCopiesAttributesLater(true);
            value = wrapper;
            break;
        }
        }
        m_dict->set(vm, name, value);
    };
    bool hasComparison = false;
    for (auto& def : methods) {
        add(def);
        static_assert(static_cast<unsigned>(NameID::Ge) - static_cast<unsigned>(NameID::Lt) == 5);
        hasComparison |= def.knownOfName.id >= NameID::Lt && def.knownOfName.id <= NameID::Ge;
    }
    // In CPython one function of a type does all six comparisons, so a type with a method for one has methods for all.
    if (hasComparison) {
        for (auto& comparison : defaultComparisons()) {
            if (!m_dict->get(rt.nameIn(comparison)))
                add(comparison);
        }
    }
    didChange(vm, Change::HowAttributesAreRead);
}

void PyType::addGetSets(JSGlobalObject* globalObject, std::span<const NativeGetSet> getsets)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    for (auto& def : getsets)
        m_dict->set(vm, rt.nameIn(def), PyGetSetDescriptor::create(globalObject, def, this));
    didChange(vm);
}

bool PyType::isSubtypeOf(const PyType* other) const
{
    // The mro() of a metaclass may have put the type anywhere, or left it out.
    if (PyTuple* mro = m_mro.get()) {
        for (unsigned i = 0; i < mro->length(); ++i) {
            if (mro->at(i).asCell() == other)
                return true;
        }
        return false;
    }
    // The MRO is still being worked out.
    for (const PyType* type = this; type; type = type->base()) {
        if (type == other)
            return true;
    }
    return false;
}

std::atomic<uint64_t> PyType::s_timesLookupRanCode;

JSValue PyType::lookupSlow(JSString* name)
{
    PyTuple* mro = m_mro.get();
    if (!mro)
        return { };
    VM& vm = this->vm();
    uint32_t numberOfChanges = m_numberOfChanges;
    JSValue value;
    for (unsigned i = 0; !value && i < mro->length(); ++i) {
        PyDict* dict = uncheckedDowncast<PyType>(mro->at(i).asCell())->dict();
        if (dict->hasOnlyStringKeys()) [[likely]] {
            value = dict->get(name);
            continue;
        }
        // type("X", (), { key: 1 }): the key is asked whether it equals the name. What it raises is lost, the name counts as not found,
        // and nothing is remembered, as in CPython.
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return { };
        JSGlobalObject* globalObject = realm();
        s_timesLookupRanCode.fetch_add(1, std::memory_order_relaxed);
        bool hasRaised = false;
        bool wasTerminated = false;
        {
            // An exception may be on its way.
            SuspendExceptionScope suspended(vm);
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            value = dict->get(globalObject, name);
            if (Exception* exception = scope.exception()) [[unlikely]] {
                hasRaised = true;
                wasTerminated = vm.isTerminationException(exception);
                if (wasTerminated)
                    scope.clearException();
                else
                    takePendingException(globalObject);
            }
        }
        // In place of whatever was on its way.
        if (wasTerminated) [[unlikely]]
            vm.throwTerminationException();
        if (hasRaised) [[unlikely]]
            return { };
    }
    // And it may have changed the type, or its bases.
    if (m_numberOfChanges != numberOfChanges) [[unlikely]]
        return value;
    StringImpl* impl = name->tryGetValueImpl();
    LookupCacheEntry& entry = lookupCacheEntry(impl);
    entry.impl = impl;
    entry.name.set(vm, this, name);
    entry.value.set(vm, this, value);
    return value;
}

JSValue PyType::lookupKey(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = this->vm();
    if (!m_mro || vm.hasPendingTerminationException()) [[unlikely]]
        return { };
    s_timesLookupRanCode.fetch_add(1, std::memory_order_relaxed);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    int64_t hash = Python::hash(globalObject, name);
    // (Which may have given the class other bases. What an __eq__ does to them comes too late.)
    PyTuple* mro = m_mro.get();
    JSValue value;
    for (unsigned i = 0; !value && !scope.exception() && i < mro->length(); ++i)
        value = uncheckedDowncast<PyType>(mro->at(i).asCell())->dict()->get(globalObject, name, hash);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        if (!vm.isTerminationException(exception))
            takePendingException(globalObject);
        return { };
    }
    return value;
}

JSValue PyType::lookupSpecialSlow(Special special)
{
    Runtime& rt = Runtime::from(realm());
    JSValue value;
    if (special != Special::GetAttr || !m_hasGivenUpGetAttr) {
        JSString* name = rt.name(special);
        value = lookup(name);
        if (special == Special::GetAttr && !value)
            m_hasGivenUpGetAttr = true;
        else {
            // What that has not remembered is not remembered here either.
            StringImpl* impl = name->tryGetValueImpl();
            if (LookupCacheEntry& entry = lookupCacheEntry(impl); entry.impl != impl || entry.value.get() != value) [[unlikely]]
                return value;
        }
    }
    unsigned index = static_cast<unsigned>(special);
    m_specials[index].set(rt.vm(), this, value);
    m_specialStates[index] = PyFunction::is(value) ? SpecialState::Function : SpecialState::Known;
    if (auto* native = PyNativeFunction::tryFrom(value); native && (native->kind() == NativeKind::Method || native->kind() == NativeKind::SlotWrapper)) {
        auto& signature = native->def().signature;
        bool takesAllOrNothing = signature.hasSelf && !signature.isVariadic && signature.minPositional == signature.parameterCount && signature.maxPositional == signature.parameterCount;
        // A class may have the method of a type it has nothing to do with: class C: __len__ = list.__len__
        if (takesAllOrNothing && signature.parameterCount <= 2 && !(native->def().flags & (DescriptorGet | OnWipedStack | MayWait)) && native->ownerType() && isSubtypeOf(native->ownerType()))
            m_specialStates[index] = static_cast<SpecialState>(static_cast<unsigned>(SpecialState::NativeTakingNone) + signature.parameterCount);
    }
    return value;
}

// Null: the class is gone.
static PyType* subclassInEntry(JSValue entry)
{
    JSCell* cell = entry.asCell();
    if (auto* tuple = PyTuple::tryFrom(cell))
        cell = tuple->at(0).asCell();
    if (!PyType::is(cell))
        cell = uncheckedDowncast<PyWeakRef>(cell)->referent();
    return cell ? uncheckedDowncast<PyType>(cell) : nullptr;
}

uint32_t PyType::takeVersion()
{
    static std::atomic<uint32_t> last;
    if (!m_versionsLeft || m_isBeingToldOfChange)
        return 0;
    uint32_t version = last.load(std::memory_order_relaxed);
    do {
        if (version == std::numeric_limits<uint32_t>::max()) [[unlikely]]
            return 0;
    } while (!last.compare_exchange_weak(version, version + 1, std::memory_order_relaxed));
    --m_versionsLeft;
    m_version = version + 1;
    return m_version;
}

Structure* createStructureOfInstances(VM&, JSGlobalObject*, Runtime&, unsigned inlineCapacity); // PyObject.cpp

Structure* PyType::createInstanceStructure(VM& vm)
{
    JSGlobalObject* globalObject = realm();
    // What __slots__ names has its place in the object. Storage on the side costs an allocation, and an assignment that the
    // interpreter cannot do by itself, for every instance.
    unsigned slotCount = 0;
    for (PyType* type = this; type; type = type->base()) {
        if (PyTuple* slots = type->slots())
            slotCount += slots->length();
    }
    unsigned capacity = std::max(PyInstance::inlineCapacity, PyInstance::numberOfFields + slotCount);
    if (has(TypeFlag::HasDict)) {
        // So has what the methods of the class and of its bases assign to `self`, which the compiler has listed. (An object of JS is given
        // room for what its constructor assigns to `this`: StaticPropertyAnalyzer, ObjectAllocationProfile.) Not what a slot, a property
        // or another data descriptor takes. Instances are made where no code of the program may run, so this is not lookup().
        UncheckedKeyHashSet<StringImpl*> names;
        Runtime& rt = Runtime::from(globalObject);
        auto find = [](PyType* type, JSString* name) {
            JSValue found;
            PyTuple* mro = type->mro();
            for (unsigned i = 0; !found && mro && i < mro->length(); ++i)
                found = uncheckedDowncast<PyType>(mro->at(i).asCell())->dict()->get(name);
            return found;
        };
        auto isTakenByDescriptor = [&](JSString* name) {
            JSValue found = find(this, name);
            if (!isPyObject(found))
                return false;
            PyType* itsType = uncheckedDowncast<PyObject>(found.asCell())->type();
            return find(itsType, rt.name(Special::Set)) || find(itsType, rt.name(Special::Delete));
        };
        PyTuple* mro = this->mro(); // Null: it is being worked out.
        for (unsigned i = 0; mro && i < mro->length(); ++i) {
            PyTuple* listed = PyTuple::tryFrom(uncheckedDowncast<PyType>(mro->at(i).asCell())->dict()->get(rt.names.dStaticAttributes()));
            for (unsigned j = 0; listed && j < listed->length(); ++j) {
                JSValue name = listed->at(j);
                if (name.isString() && !asString(name)->isRope() && !names.contains(asString(name)->tryGetValueImpl()) && !isTakenByDescriptor(asString(name)))
                    names.add(asString(name)->tryGetValueImpl());
            }
        }
        capacity = PyInstance::numberOfFields + slotCount + std::max<unsigned>(PyInstance::inlineCapacity - PyInstance::numberOfFields, names.size());
        // Cells come in certain sizes. What is left over in one is room as well, as in ObjectAllocationProfile::initializeProfile(). The
        // largest size is one that can be used to the full.
        auto roomInCell = [](unsigned capacity) -> unsigned {
            return (MarkedSpace::optimalSizeFor(PyInstance::allocationSize(capacity)) - PyInstance::allocationSize(0)) / sizeof(WriteBarrierBase<Unknown>);
        };
        capacity = std::min(capacity, JSFinalObject::maxInlineCapacity);
        while (roomInCell(capacity) > JSFinalObject::maxInlineCapacity)
            --capacity;
        capacity = roomInCell(capacity);
    }
    Structure* structure = createStructureOfInstances(vm, globalObject, Runtime::from(globalObject), std::min(capacity, JSFinalObject::maxInlineCapacity));
    m_instanceStructure.set(vm, this, structure);
    return structure;
}

void PyType::didChange(VM& vm, Change change)
{
    // Two types may each have the other in an MRO of their own making.
    if (m_isBeingToldOfChange)
        return;
    SetForScope isBeingTold(m_isBeingToldOfChange, true);
    m_version = 0;
    ++m_numberOfChanges;
    m_construction = Construction::Unknown;
    if (change == Change::HowAttributesAreRead)
        m_hasGivenUpGetAttr = false;
    // What the caches held must not be kept alive by them: it may be what was just taken out of the class.
    m_specialStates.fill(SpecialState::Unknown);
    for (auto& special : m_specials)
        special.clear();
    for (auto& entry : m_lookupCache) {
        entry.impl = nullptr;
        entry.name.clear();
        entry.value.clear();
    }
    JSValue subclasses = m_subclasses.get();
    if (!subclasses)
        return;
    JSArray* list = asList(subclasses);
    for (unsigned i = 0; i < listLength(list); ++i) {
        if (PyType* subclass = subclassInEntry(listGet(list, i)))
            subclass->didChange(vm, change);
    }
}

void PyType::appendSubclassesTo(MarkedArgumentBuffer& result) const
{
    JSValue subclasses = m_subclasses.get();
    if (!subclasses)
        return;
    JSArray* list = asList(subclasses);
    for (unsigned i = 0; i < listLength(list); ++i) {
        JSValue entry = listGet(list, i);
        if (PyTuple::is(entry))
            continue;
        if (PyType* subclass = subclassInEntry(entry); subclass && !subclass->has(TypeFlag::Internal))
            result.append(subclass);
    }
}

void PyType::addSubclass(JSGlobalObject* globalObject, PyType* subclass)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!m_subclasses)
        m_subclasses.set(vm, this, newList(globalObject));
    JSArray* list = asList(m_subclasses.get());
    // Now and then, what is left of the classes that are gone goes too.
    if (unsigned length = listLength(list); length >= 16 && !(length & (length - 1))) {
        for (unsigned i = length; i--;) {
            if (!subclassInEntry(listGet(list, i))) {
                listRemove(globalObject, list, i);
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
    }
    JSValue entry = subclass;
    if (subclass->isHeapType())
        entry = PyWeakRef::create(vm, rt.weakRefStructure(), rt.weakRefType(), subclass, { });
    listAppend(globalObject, list, entry);
    RETURN_IF_EXCEPTION(scope, void());
}

void PyType::addDependent(JSGlobalObject* globalObject, PyType* dependent)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    if (JSValue subclasses = m_subclasses.get()) {
        JSArray* list = asList(subclasses);
        for (unsigned i = 0; i < listLength(list); ++i) {
            if (subclassInEntry(listGet(list, i)) == dependent)
                return;
        }
    }
    addSubclass(globalObject, dependent);
    RETURN_IF_EXCEPTION(scope, void());
    JSArray* list = asList(m_subclasses.get());
    unsigned last = listLength(list) - 1;
    PyTuple* entry = PyTuple::create(globalObject, { listGet(list, last) });
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, listSet(globalObject, list, last, entry));
}

void PyType::removeSubclass(JSGlobalObject* globalObject, PyType* subclass)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!m_subclasses)
        return;
    JSArray* list = asList(m_subclasses.get());
    for (unsigned i = 0; i < listLength(list); ++i) {
        if (JSValue entry = listGet(list, i); !PyTuple::is(entry) && subclassInEntry(entry) == subclass) {
            RELEASE_AND_RETURN(scope, listRemove(globalObject, list, i));
        }
    }
}

// ---- The C3 linearization ----

PyTuple* computeMRO(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyTuple* bases = type->bases();
    unsigned baseCount = bases->length();
    for (unsigned i = 0; i < baseCount; ++i) {
        PyType* base = PyType::from(bases->at(i));
        if (!base->mro()) {
            raiseTypeError(globalObject, scope, "Cannot extend an incomplete type '"_s, base->name(), '\'');
            return nullptr;
        }
    }

    // One base: its MRO after the type.
    if (baseCount == 1) {
        PyTuple* baseMRO = PyType::from(bases->at(0))->mro();
        PyTuple* result = PyTuple::create(globalObject, baseMRO->length() + 1);
        result->initializeAt(vm, 0, type);
        for (unsigned i = 0; i < baseMRO->length(); ++i)
            result->initializeAt(vm, i + 1, baseMRO->at(i));
        return result;
    }

    for (unsigned i = 0; i < baseCount; ++i) {
        for (unsigned j = i + 1; j < baseCount; ++j) {
            if (bases->at(i) == bases->at(j)) {
                raiseTypeError(globalObject, scope, "duplicate base class "_s, PyType::from(bases->at(i))->name());
                return nullptr;
            }
        }
    }

    // Merge the MROs of the bases and the list of the bases itself. All of these are reachable from the type.
    Vector<PyTuple*, 8> sequences;
    for (unsigned i = 0; i < baseCount; ++i)
        sequences.append(PyType::from(bases->at(i))->mro());
    sequences.append(bases);
    Vector<unsigned, 8> positions;
    positions.fill(0, sequences.size());

    Vector<JSValue, 16> result;
    result.append(type);
    auto isInTail = [&](JSValue candidate) {
        for (unsigned i = 0; i < sequences.size(); ++i) {
            for (unsigned j = positions[i] + 1; j < sequences[i]->length(); ++j) {
                if (sequences[i]->at(j) == candidate)
                    return true;
            }
        }
        return false;
    };
    while (true) {
        unsigned exhausted = 0;
        bool found = false;
        for (unsigned i = 0; i < sequences.size(); ++i) {
            if (positions[i] >= sequences[i]->length()) {
                ++exhausted;
                continue;
            }
            JSValue candidate = sequences[i]->at(positions[i]);
            if (isInTail(candidate))
                continue;
            result.append(candidate);
            for (unsigned j = 0; j < sequences.size(); ++j) {
                if (positions[j] < sequences[j]->length() && sequences[j]->at(positions[j]) == candidate)
                    ++positions[j];
            }
            found = true;
            break;
        }
        if (found)
            continue;
        if (exhausted == sequences.size())
            break;

        StrBuilder message;
        message.append("Cannot create a consistent method resolution order (MRO) for bases "_s);
        Vector<JSValue, 8> remaining;
        for (unsigned i = 0; i < sequences.size(); ++i) {
            if (positions[i] < sequences[i]->length() && !remaining.contains(sequences[i]->at(positions[i])))
                remaining.append(sequences[i]->at(positions[i]));
        }
        for (unsigned i = 0; i < remaining.size(); ++i) {
            if (i)
                message.append(", "_s);
            message.append(PyType::from(remaining[i])->name());
        }
        raiseTypeError(globalObject, scope, message.toString());
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, result.span()));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
