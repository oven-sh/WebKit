/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyType.h"

#include "DeferTermination.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDescriptors.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyJSClass.h"
#include "PyJSView.h"
#include "PyList.h"
#include "PyRuntime.h"
#include "PyStr.h"
#include "PyWeakRef.h"
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {


const ClassInfo PyType::s_info = { "PyType"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyType) };

static ALWAYS_INLINE EncodedJSValue wayBackToCaller(JSGlobalObject* globalObject, CallFrame* callFrame, JSValue result)
{
    if (!lookForSignalsBehindCall(globalObject, callFrame, result)) [[unlikely]]
        return { };
    return JSValue::encode(resultForCaller(globalObject, callFrame, result));
}

JSC_DEFINE_HOST_FUNCTION(callType, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    CallArgs args(callFrame);
    if (!args.hasKeywords() && hasKeywordMarker(globalObject, callFrame)) [[unlikely]]
        return wayBackToCaller(globalObject, callFrame, callWithKeywordMarker(globalObject, callFrame));
    // What call() comes to for a class whose class is type.
    auto* type = uncheckedDowncast<PyType>(callFrame->jsCallee());
    if (type->type() == runtime(globalObject).typeType() && getVM(globalObject).isSafeToRecurseSoft()) [[likely]]
        return wayBackToCaller(globalObject, callFrame, typeCall(globalObject, type, args));
    return wayBackToCaller(globalObject, callFrame, call(globalObject, type, args));
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
    type->m_lookupCache = &Runtime::from(structure->globalObject()).typeLookupCache();
    type->takeNewLookupKey();
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
    visitor.append(thisObject->m_maker);
    visitor.append(thisObject->m_extension);
    visitor.appendValues(thisObject->m_specials.data(), numberOfSpecials);
}

DEFINE_VISIT_CHILDREN(PyType);

String PyType::name() const
{
    return stringOf(m_name.get());
}

String PyType::nameInMessages() const
{
    if (!m_module)
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
    // Nothing here raises: nothing of the program's runs, and what is allocated is small. If the host ends what runs here, that
    // waits. The type may be wanted for the first time while an exception propagates, which is set aside.
    ExceptionSetAside suspended(globalObject);
    DeferTerminationForAWhile deferred(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    String fullName = def.name;
    size_t dot = fullName.reverseFind('.');
    m_type.set(vm, this, metatype);
    setNameString(vm, internString(vm, dot == notFound ? fullName : fullName.substring(dot + 1)));
    m_flags = def.flags;
    // See the flag.
    if (!has(TypeFlag::Base) && !has(TypeFlag::NothingOfItsOwn))
        m_flags.add(TypeFlag::OwnLayout);
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

    // Nothing makes an instance of a built-in type that has not said how. (addMethods() has seen to one that has.)
    if (!m_maker && base && (base == rt.objectType() || base->has(TypeFlag::DisallowInstantiation)))
        m_flags.add(TypeFlag::DisallowInstantiation);
    if (has(TypeFlag::DisallowInstantiation))
        m_maker.clear();
    else if (!m_maker) {
        m_maker.setMayBeNull(vm, this, base->maker());
        m_looksUpNew = base->looksUpNew();
    }
    // The __new__ of object makes no cells but its own.
    RELEASE_ASSERT(!has(TypeFlag::OwnLayout) || !isMadeBy(objectNew));
}

void PyType::takeMakerOfBase(VM& vm)
{
    m_flags.remove(TypeFlag::DisallowInstantiation);
    m_maker.setMayBeNull(vm, this, m_base->maker());
    m_looksUpNew = m_base->looksUpNew();
    m_construction = Construction::Unknown;
}

bool PyType::updateMaker()
{
    m_construction = Construction::Unknown;
    // In a new class nothing has been looked up yet, and this may never be asked for again: lookup() would remember it, at a price. So the dicts are
    // asked as they come. (Not what __base__ remembers: a class made after something was written into a dict behind the back of its base sees that.)
    PyTuple* mro = this->mro();
    JSString* name = runtime(realm()).name(Special::New);
    JSValue found;
    bool asksNoKeys = true;
    for (unsigned i = 0; mro && i < mro->length() && !found && asksNoKeys; ++i) {
        PyDict* dict = uncheckedDowncast<PyType>(mro->at(i).asCell())->dict();
        asksNoKeys = dict->hasOnlyStringKeys();
        if (asksNoKeys)
            found = dict->get(name);
    }
    bool hasRaised = false;
    if (!asksNoKeys)
        found = lookupSlow(name, AsksKeys::Yes, &hasRaised);
    if (!found) {
        m_maker.clear();
        m_looksUpNew = false;
        return !hasRaised;
    }
    // The __new__ of a built-in type, whichever, stands for "as it was".
    if (auto* native = PyNativeFunction::tryFrom(found); native && native->kind() == NativeKind::New)
        return true;
    m_maker.clear();
    m_looksUpNew = true;
    return true;
}

void PyType::updateMakerAgain(VM& vm)
{
    // What it took from its base it keeps, as a class in the list of its base would: the base may have lost its own meanwhile.
    PyNativeFunction* maker = m_maker.get();
    bool looksUpNew = m_looksUpNew;
    if (updateMaker()) [[likely]]
        return;
    m_maker.setMayBeNull(vm, this, maker);
    m_looksUpNew = looksUpNew;
}

JSValue PyType::updateMakers()
{
    // What is left to do is in a list, not on the stack: classes derive from one another to any depth. The last is next: all that derives from a class comes
    // before the class beside it, as in CPython. Each is seen to once, however many ways lead to it, unless a key has been asked. (Not those that only have this
    // one in an MRO of their own making.)
    updateMaker();
    if (!m_subclasses)
        return { };
    MarkedArgumentBuffer left;
    appendSubclassesTo(left, SubclassOrder::NewestFirst);
    UncheckedKeyHashSet<PyType*> seen;
    // CPython comes to a class by every way that leads to it. That shows where a key has been asked: by the next way it is asked again, and may say something else.
    UncheckedKeyHashSet<PyType*> askedInVain;
    // A key may let go of a class that has been seen to, collect, and make classes: one in the cell of that class would count as seen to.
    MarkedArgumentBuffer keptAlive;
    JSString* name = runtime(realm()).name(Special::New);
    while (!left.isEmpty()) {
        PyType* type = PyType::from(left.takeLast());
        bool isFirstTime = seen.add(type).isNewEntry;
        if (!isFirstTime && !askedInVain.remove(type))
            continue;
        keptAlive.append(type);
        PyDict* dict = type->dict();
        bool hasRaised = false;
        JSValue raised;
        JSValue own = dict->hasOnlyStringKeys() ? dict->get(name) : type->getAskingKeys(dict, name, hasRaised, &raised);
        // A key of its dict raises when it is asked whether it is the name: there CPython gives up (and leaves the exception for whoever comes next). That class,
        // and all whose turn has not come, stay as they were.
        if (hasRaised) [[unlikely]]
            return raised;
        if (own) {
            // A key may have said so.
            if (!dict->hasOnlyStringKeys()) [[unlikely]]
                seen.remove(type);
            continue;
        }
        if (!type->updateMaker()) [[unlikely]]
            askedInVain.add(type);
        if (isFirstTime)
            type->appendSubclassesTo(left, SubclassOrder::NewestFirst);
    }
    return { };
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
        case NativeKind::New: {
            auto* function = PyNativeFunction::create(globalObject, def, NativeKind::New, this, this, name);
            m_maker.set(vm, this, function);
            value = function;
            break;
        }
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
std::atomic<uint64_t> PyType::s_lastLookupKey;

WTF_MAKE_TZONE_ALLOCATED_IMPL(TypeLookupCache);

void TypeLookupCache::removeUnmarked(Heap& heap)
{
    uint64_t runs = heap.collectionScope() == CollectionScope::Eden ? m_written : ~0ull;
    m_written = 0;
#if ASSERT_ENABLED
    for (unsigned i = 0; i < size; ++i) {
        if (!(runs >> (i / entriesPerBit) & 1))
            ASSERT(!m_entries[i].value || !m_entries[i].value.isCell() || heap.isMarked(m_entries[i].value.asCell()));
    }
#endif
    // (The text is let go of when the entry is taken for another name: not here, which may be the collector's thread.)
    for (; runs; runs &= runs - 1) {
        for (Entry& entry : std::span { m_entries }.subspan(WTF::ctz(runs) * entriesPerBit, entriesPerBit)) {
            if (entry.value && entry.value.isCell() && !heap.isMarked(entry.value.asCell())) {
                entry.key = 0;
                entry.value = JSValue();
            }
        }
    }
}

void PyType::takeNewLookupKey()
{
    m_lookupKey = (s_lastLookupKey.fetch_add(1, std::memory_order_relaxed) + 1) * lookupKeyFactor;
}

// type("X", (), { key: 1 }): the key is asked whether it equals the name. What it raises is lost, unless the caller wants it (`raised`).
JSValue PyType::getAskingKeys(PyDict* dict, JSString* name, bool& hasRaised, JSValue* raised)
{
    VM& vm = this->vm();
    hasRaised = true;
    if (vm.hasPendingTerminationException()) [[unlikely]]
        return { };
    JSGlobalObject* globalObject = realm();
    m_hasAskedKeys = true;
    s_timesLookupRanCode.fetch_add(1, std::memory_order_relaxed);
    hasRaised = false;
    bool wasTerminated = false;
    JSValue value;
    {
        // An exception may be on its way.
        ExceptionSetAside suspended(globalObject);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        value = dict->get(globalObject, name);
        if (Exception* exception = scope.exception()) [[unlikely]] {
            hasRaised = true;
            wasTerminated = vm.isTerminationException(exception);
            if (wasTerminated)
                scope.clearException();
            else if (JSValue taken = takePendingException(globalObject); raised)
                *raised = taken;
        }
    }
    // In place of whatever was on its way.
    if (wasTerminated) [[unlikely]]
        vm.throwTerminationException();
    return hasRaised ? JSValue() : value;
}

JSValue PyType::lookupSlow(JSString* name, AsksKeys asksKeys, bool* hasRaisedResult)
{
    PyTuple* mro = m_mro.get();
    if (!mro)
        return { };
    VM& vm = this->vm();
    // The collector asks what is callable (SamplingProfiler::processUnverifiedStackTraces()), on whatever thread it runs. It is told what is
    // there: to remember is to let go of another name, which takes the VM's table of atoms. As in SymbolTable::materializeCachedEntries().
    bool isForCollector = vm.heap.currentThreadIsDoingGCWork();
    if (isForCollector) [[unlikely]]
        asksKeys = AsksKeys::No;
    bool hasPassedOverKeys = false;
    uint64_t key = m_lookupKey;
    JSValue value;
    for (unsigned i = 0; !value && i < mro->length(); ++i) {
        PyDict* dict = uncheckedDowncast<PyType>(mro->at(i).asCell())->dict();
        if (dict->hasOnlyStringKeys()) [[likely]] {
            value = dict->get(name);
            continue;
        }
        if (asksKeys == AsksKeys::No) {
            value = dict->get(name);
            hasPassedOverKeys = true;
            continue;
        }
        // If one raises, the name counts as not found, and nothing is remembered, as in CPython.
        bool hasRaised = false;
        value = getAskingKeys(dict, name, hasRaised);
        if (hasRaised) [[unlikely]] {
            if (hasRaisedResult)
                *hasRaisedResult = true;
            return { };
        }
    }
    // And it may have changed the type, or its bases.
    if (m_lookupKey != key || isForCollector || hasPassedOverKeys) [[unlikely]]
        return value;
    // The table keeps the text. (The limit is CPython's.)
    StringImpl* impl = name->tryGetValueImpl();
    if (impl->length() > 100) [[unlikely]]
        return value;
    m_lookupCache->remember(key, impl, value);
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

JSValue PyType::lookupSpecialSlow(Special special, AsksKeys asksKeys)
{
    Runtime& rt = Runtime::from(realm());
    JSValue value;
    if (special != Special::GetAttr || !m_hasGivenUpGetAttr) {
        JSString* name = rt.name(special);
        value = asksKeys == AsksKeys::Yes ? lookup(name) : lookupSlow(name, asksKeys);
        // See lookupSlow().
        if (rt.vm().heap.currentThreadIsDoingGCWork()) [[unlikely]]
            return value;
        if (special == Special::GetAttr && !value)
            m_hasGivenUpGetAttr = true;
        else {
            // What that has not remembered is not remembered here either.
            StringImpl* impl = name->tryGetValueImpl();
            if (TypeLookupCache::Entry& entry = m_lookupCache->entryFor(m_lookupKey, impl); entry.key != m_lookupKey || entry.impl != impl || entry.value != value) [[unlikely]]
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
    if (!m_versionsLeft)
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
    ASSERT(!has(TypeFlag::InstancesHaveDataOfC));
    JSGlobalObject* globalObject = realm();
    if (has(TypeFlag::ExceptionSubclass)) {
        Structure* structure = PyException::createStructureOfInstances(globalObject, this);
        m_instanceStructure.set(vm, this, structure);
        return structure;
    }
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

void PyType::forgetWhatIsRemembered(Change change)
{
    m_version = 0;
    takeNewLookupKey();
    m_construction = Construction::Unknown;
    // (It goes by the bases.)
    if (has(TypeFlag::ExceptionSubclass))
        m_instanceStructure.clear();
    if (change == Change::HowAttributesAreRead)
        m_hasGivenUpGetAttr = false;
    // What the caches held must not be kept alive by them: it may be what was just taken out of the class.
    m_specialStates.fill(SpecialState::Unknown);
    for (auto& special : m_specials)
        special.clear();
    m_hasAskedKeys = false;
}

void PyType::didChange(VM&, Change change)
{
    forgetWhatIsRemembered(change);
    if (!m_subclasses)
        return;
    // Those that are left to tell are in a list, not on the stack: classes derive from one another to any depth. Each is told once, however many ways lead to
    // it, and two types may each have the other in an MRO of their own making.
    AssertNoGC assertNoGC;
    Vector<PyType*, 16> told;
    told.append(this);
    m_isInWalk = true;
    for (size_t next = 0; next < told.size(); ++next) {
        JSValue subclasses = told[next]->m_subclasses.get();
        if (!subclasses)
            continue;
        JSArray* list = asList(subclasses);
        for (unsigned i = 0; i < listLength(list); ++i) {
            PyType* subclass = subclassInEntry(listGet(list, i));
            if (!subclass || subclass->m_isInWalk)
                continue;
            subclass->m_isInWalk = true;
            subclass->forgetWhatIsRemembered(change);
            told.append(subclass);
        }
    }
    for (PyType* type : told)
        type->m_isInWalk = false;
}

bool PyType::hasAmongBases(const PyType* other)
{
    AssertNoGC assertNoGC;
    Vector<PyType*, 16> reached;
    reached.append(this);
    m_isInWalk = true;
    bool isFound = false;
    for (size_t next = 0; !isFound && next < reached.size(); ++next) {
        isFound = reached[next] == other;
        PyTuple* bases = reached[next]->bases();
        for (unsigned i = 0; bases && i < bases->length(); ++i) {
            PyType* base = PyType::from(bases->at(i));
            if (base->m_isInWalk)
                continue;
            base->m_isInWalk = true;
            reached.append(base);
        }
    }
    for (PyType* type : reached)
        type->m_isInWalk = false;
    return isFound;
}

bool PyType::appendSubclassesTo(MarkedArgumentBuffer& result, SubclassOrder order, Dependents dependents) const
{
    JSValue subclasses = m_subclasses.get();
    if (!subclasses)
        return false;
    JSArray* list = asList(subclasses);
    unsigned length = listLength(list);
    bool hasDependents = false;
    for (unsigned i = 0; i < length; ++i) {
        JSValue entry = listGet(list, order == SubclassOrder::OldestFirst ? i : length - 1 - i);
        if (PyTuple::is(entry)) {
            hasDependents = true;
            if (dependents == Dependents::No)
                continue;
        }
        if (PyType* subclass = subclassInEntry(entry); subclass && !subclass->has(TypeFlag::Internal))
            result.append(subclass);
    }
    return hasDependents;
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

bool PyType::hasInList(PyType* subclass) const
{
    if (!m_subclasses)
        return false;
    JSArray* list = asList(m_subclasses.get());
    for (unsigned i = 0; i < listLength(list); ++i) {
        if (JSValue entry = listGet(list, i); !PyTuple::is(entry) && subclassInEntry(entry) == subclass)
            return true;
    }
    return false;
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
