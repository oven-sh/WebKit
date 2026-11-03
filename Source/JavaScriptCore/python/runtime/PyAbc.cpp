/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "JSWeakSet.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include "PySet.h"
#include "PyWeakRef.h"
#include "WeakMapImplInlines.h"

namespace JSC {
namespace Python {

// The module _abc: what abc.ABCMeta is made of. (Modules/_abc.c in CPython is the reference.)
//
// An abstract base class remembers the classes registered with it, and those it has found to be, or not to be, subclasses of it,
// without keeping any of them alive. CPython has sets of weak references for that, each with a callback that takes it out of
// its set. Here they are the WeakSets of JS, which the collector itself tends.

enum AbcStateField : unsigned { AbcStateDataType, AbcStateImplName, AbcStateSubclassesName, AbcStateFlagsName, AbcStateFieldCount };
// number: how many times a class has been registered with any ABC, after which "is not a subclass" may no longer hold.

enum AbcDataField : unsigned { AbcDataRegistry, AbcDataCache, AbcDataNegativeCache, AbcDataFieldCount };
// number: what the count above was when the negative cache was last emptied.

static JSValue abcDataNew(JSGlobalObject*, JSValue cls, const CallArgs&);

static constexpr NativeDef abcDataMethods[] = {
    PY_NEW(abcDataNew, "($type, *args, **kwargs)"),
};

static constexpr TypeDef abcDataTypeDef { "_abc._abc_data"_s, { }, abcDataMethods, { }, "Internal state held by ABC machinery."_s };

static PyRecord* abcState(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (JSValue state = rt.slot(Runtime::Slot::Abc)) [[likely]]
        return uncheckedDowncast<PyRecord>(state.asCell());
    VM& vm = rt.vm();
    PyRecord* state = PyRecord::create(globalObject, rt.objectType(), AbcStateFieldCount);
    rt.setSlot(Runtime::Slot::Abc, state);
    state->set(vm, AbcStateDataType, rt.typeFor(abcDataTypeDef));
    state->set(vm, AbcStateImplName, internString(vm, "_abc_impl"_s));
    state->set(vm, AbcStateSubclassesName, internString(vm, "__subclasses__"_s));
    state->set(vm, AbcStateFlagsName, internString(vm, "__abc_tpflags__"_s));
    return state;
}

static JSValue abcDataNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    PyRecord* data = PyRecord::create(globalObject, PyType::from(cls), AbcDataFieldCount);
    data->number = abcState(globalObject)->number;
    return data;
}

// cls._abc_impl. Null: an exception.
static PyRecord* abcImpl(JSGlobalObject* globalObject, PyRecord* state, JSValue cls)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue impl = getAttr(globalObject, cls, asString(state->get(AbcStateImplName)));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!PyRecord::is(impl) || JSValue(uncheckedDowncast<PyRecord>(impl.asCell())->type()) != state->get(AbcStateDataType)) {
        raiseTypeError(globalObject, scope, "_abc_impl is set to a wrong type"_s);
        return nullptr;
    }
    return uncheckedDowncast<PyRecord>(impl.asCell());
}

static ALWAYS_INLINE bool abcIsIn(PyRecord* impl, AbcDataField field, JSValue object)
{
    JSValue set = impl->get(field);
    return set && object.isObject() && uncheckedDowncast<JSWeakSet>(set.asCell())->has(object.asCell());
}

static void abcAddTo(JSGlobalObject* globalObject, PyRecord* impl, AbcDataField field, PyType* cls)
{
    VM& vm = getVM(globalObject);
    JSValue set = impl->get(field);
    if (!set) {
        set = JSWeakSet::create(vm, globalObject->weakSetStructure());
        impl->set(vm, field, set);
    }
    uncheckedDowncast<JSWeakSet>(set.asCell())->add(vm, cls);
}

static void abcTakeSnapshot(PyRecord* impl, AbcDataField field, MarkedArgumentBuffer& classes)
{
    if (JSValue set = impl->get(field))
        uncheckedDowncast<JSWeakSet>(set.asCell())->takeSnapshot(classes);
}

// ---- Setting a class up ----

// getattr(value, "__isabstractmethod__", False)
static bool abcIsAbstract(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue flag = getAttrIfPresent(globalObject, value, runtime(globalObject).names.dIsAbstractMethod());
    RETURN_IF_EXCEPTION(scope, false);
    if (!flag)
        return false;
    RELEASE_AND_RETURN(scope, isTrue(globalObject, flag));
}

// Sets cls.__abstractmethods__. False: an exception.
static bool abcComputeAbstractMethods(JSGlobalObject* globalObject, JSValue cls)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PySet* abstracts = PySet::createFrozen(globalObject);

    // Those the class itself defines. Asking whether one is abstract may change the namespace, so this goes by a copy.
    JSValue ns = getAttr(globalObject, cls, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, false);
    JSValue itemsView = callMethod(globalObject, ns, rt.names.items());
    RETURN_IF_EXCEPTION(scope, false);
    JSArray* items = listFromIterable(globalObject, itemsView);
    RETURN_IF_EXCEPTION(scope, false);
    for (unsigned i = 0; i < listLength(items); ++i) {
        JSValue item = listGet(items, i);
        JSValue key, value;
        if (auto* pair = PyTuple::tryFrom(item); pair && pair->length() == 2) [[likely]] {
            key = pair->at(0);
            value = pair->at(1);
        } else {
            JSArray* pairAsList = listFromIterable(globalObject, item);
            if (scope.exception()) {
                if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                    raiseTypeError(globalObject, scope, "items() returned non-iterable"_s);
                return false;
            }
            if (listLength(pairAsList) != 2) {
                raiseTypeError(globalObject, scope, "items() returned item which size is not 2"_s);
                return false;
            }
            key = listGet(pairAsList, 0);
            value = listGet(pairAsList, 1);
        }
        bool isAbstract = abcIsAbstract(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (isAbstract) {
            abstracts->add(globalObject, key);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }

    // Those it inherits and has not overridden.
    JSValue basesValue = getAttr(globalObject, cls, rt.names.dBases());
    RETURN_IF_EXCEPTION(scope, false);
    auto* bases = PyTuple::tryFrom(basesValue);
    if (!bases) {
        raiseTypeError(globalObject, scope, "__bases__ is not tuple"_s);
        return false;
    }
    for (unsigned i = 0; i < bases->length(); ++i) {
        JSValue baseAbstracts = getAttrIfPresent(globalObject, bases->at(i), rt.names.dAbstractMethods());
        RETURN_IF_EXCEPTION(scope, false);
        if (!baseAbstracts)
            continue;
        forEach(globalObject, baseAbstracts, [&](JSValue key) {
            JSString* name = attributeName(globalObject, key);
            RETURN_IF_EXCEPTION(scope, false);
            JSValue value = getAttrIfPresent(globalObject, cls, name);
            RETURN_IF_EXCEPTION(scope, false);
            if (!value)
                return true;
            bool isAbstract = abcIsAbstract(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            if (isAbstract)
                abstracts->add(globalObject, key);
            return true;
        });
        RETURN_IF_EXCEPTION(scope, false);
    }

    setAttr(globalObject, cls, rt.names.dAbstractMethods(), abstracts);
    return !scope.exception();
}

// Py_TPFLAGS_SEQUENCE and Py_TPFLAGS_MAPPING, which is what collections.abc writes in __abc_tpflags__.
static constexpr int64_t abcSequenceFlag = 1 << 5;
static constexpr int64_t abcMappingFlag = 1 << 6;

static JSValue abcInit(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* state = abcState(globalObject);
    if (!abcComputeAbstractMethods(globalObject, self))
        return { };
    PyRecord* data = PyRecord::create(globalObject, PyType::from(state->get(AbcStateDataType)), AbcDataFieldCount);
    data->number = state->number;
    setAttr(globalObject, self, asString(state->get(AbcStateImplName)), data);
    RETURN_IF_EXCEPTION(scope, { });

    // How collections.abc.Sequence and Mapping say what they are to pattern matching.
    auto* cls = PyType::tryFrom(self);
    if (!cls)
        return jsUndefined();
    JSValue flags = cls->dict()->take(vm, asString(state->get(AbcStateFlagsName)));
    if (!flags)
        return jsUndefined();
    cls->didChange(vm);
    if (!isExactInt(flags))
        return jsUndefined();
    auto value = tryInt64(flags);
    if (!value)
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
    if ((*value & abcSequenceFlag) && (*value & abcMappingFlag))
        return raiseTypeError(globalObject, scope, "__abc_tpflags__ cannot be both Py_TPFLAGS_SEQUENCE and Py_TPFLAGS_MAPPING"_s);
    if (*value & abcSequenceFlag)
        cls->add(TypeFlag::Sequence);
    if (*value & abcMappingFlag)
        cls->add(TypeFlag::Mapping);
    return jsUndefined();
}

// The class and all that derive from it are sequences, or mappings, from now on. What is written in C++ knows what it is.
static void abcSetCollectionFlag(PyType* cls, TypeFlag flag)
{
    TypeFlag other = flag == TypeFlag::Sequence ? TypeFlag::Mapping : TypeFlag::Sequence;
    if (!cls->isHeapType() || cls->has(TypeFlag::Immutable) || (cls->has(flag) && !cls->has(other)))
        return;
    cls->remove(other);
    cls->add(flag);
    MarkedArgumentBuffer subclasses;
    cls->appendSubclassesTo(subclasses);
    for (unsigned i = 0; i < subclasses.size(); ++i)
        abcSetCollectionFlag(PyType::from(subclasses.at(i)), flag);
}

static JSValue abcRegister(JSGlobalObject* globalObject, JSValue self, JSValue subclassValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* subclass = PyType::tryFrom(subclassValue);
    if (!subclass)
        return raiseTypeError(globalObject, scope, "Can only register classes"_s);
    bool isAlready = isSubclass(globalObject, subclass, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isAlready)
        return subclass;
    // After the test above, so that X.register(X) does nothing.
    bool isCycle = isSubclass(globalObject, self, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    if (isCycle)
        return raiseRuntimeError(globalObject, scope, "Refusing to create an inheritance cycle"_s);
    PyRecord* state = abcState(globalObject);
    PyRecord* impl = abcImpl(globalObject, state, self);
    RETURN_IF_EXCEPTION(scope, { });
    abcAddTo(globalObject, impl, AbcDataRegistry, subclass);
    ++state->number;
    if (auto* cls = PyType::tryFrom(self)) {
        if (cls->has(TypeFlag::Sequence))
            abcSetCollectionFlag(subclass, TypeFlag::Sequence);
        else if (cls->has(TypeFlag::Mapping))
            abcSetCollectionFlag(subclass, TypeFlag::Mapping);
    }
    return subclass;
}

// ---- isinstance, issubclass ----

static JSValue abcInstanceCheck(JSGlobalObject* globalObject, JSValue self, JSValue instance)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* state = abcState(globalObject);
    PyRecord* impl = abcImpl(globalObject, state, self);
    RETURN_IF_EXCEPTION(scope, { });

    PyType* type = rt.typeOf(instance);
    JSValue subclass = getAttrIfPresent(globalObject, instance, rt.names.dClass());
    RETURN_IF_EXCEPTION(scope, { });
    if (!subclass)
        subclass = type;
    if (abcIsIn(impl, AbcDataCache, subclass))
        return jsBoolean(true);
    JSString* subclassCheck = rt.name(Special::SubclassCheck);
    if (subclass == JSValue(type)) {
        if (impl->number == state->number && abcIsIn(impl, AbcDataNegativeCache, subclass))
            return jsBoolean(false);
        RELEASE_AND_RETURN(scope, callMethod(globalObject, self, subclassCheck, subclass));
    }
    JSValue result = callMethod(globalObject, self, subclassCheck, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    bool isSubclassByClass = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (isSubclassByClass)
        return result;
    RELEASE_AND_RETURN(scope, callMethod(globalObject, self, subclassCheck, type));
}

static JSValue abcSubclassCheck(JSGlobalObject* globalObject, JSValue self, JSValue subclassValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* subclass = PyType::tryFrom(subclassValue);
    if (!subclass)
        return raiseTypeError(globalObject, scope, "issubclass() arg 1 must be a class"_s);
    PyRecord* state = abcState(globalObject);
    PyRecord* impl = abcImpl(globalObject, state, self);
    RETURN_IF_EXCEPTION(scope, { });

    auto answer = [&](bool result) {
        abcAddTo(globalObject, impl, result ? AbcDataCache : AbcDataNegativeCache, subclass);
        return jsBoolean(result);
    };

    if (abcIsIn(impl, AbcDataCache, subclass))
        return jsBoolean(true);
    if (impl->number < state->number) {
        impl->set(vm, AbcDataNegativeCache, { });
        impl->number = state->number;
    } else if (abcIsIn(impl, AbcDataNegativeCache, subclass))
        return jsBoolean(false);

    JSValue ok = callMethod(globalObject, self, rt.names.dSubclassHook(), subclass);
    RETURN_IF_EXCEPTION(scope, { });
    if (ok.isBoolean())
        return answer(ok.asBoolean());
    if (ok != rt.notImplemented())
        return raiseAssertionError(globalObject, scope, "__subclasshook__ must return either False, True, or NotImplemented"_s);

    if (auto* cls = PyType::tryFrom(self); cls && subclass->isSubtypeOf(cls))
        return answer(true);

    // A subclass of a class that is registered?
    if (abcIsIn(impl, AbcDataRegistry, subclass))
        return jsBoolean(true);
    {
        MarkedArgumentBuffer registered;
        abcTakeSnapshot(impl, AbcDataRegistry, registered);
        for (unsigned i = 0; i < registered.size(); ++i) {
            bool found = isSubclass(globalObject, subclass, registered.at(i));
            RETURN_IF_EXCEPTION(scope, { });
            if (found)
                return answer(true);
        }
    }

    // A subclass of a subclass?
    JSValue subclasses = callMethod(globalObject, self, asString(state->get(AbcStateSubclassesName)));
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = tryAsList(subclasses);
    if (!list)
        return raiseTypeError(globalObject, scope, "__subclasses__() must return a list"_s);
    for (unsigned i = 0; i < listLength(list); ++i) {
        bool found = isSubclass(globalObject, subclass, listGet(list, i));
        RETURN_IF_EXCEPTION(scope, { });
        if (found)
            return answer(true);
    }
    return answer(false);
}

// ---- For tests and debugging ----

static JSValue abcResetRegistry(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* impl = abcImpl(globalObject, abcState(globalObject), self);
    if (!impl)
        return { };
    impl->set(getVM(globalObject), AbcDataRegistry, { });
    return jsUndefined();
}

static JSValue abcResetCaches(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* impl = abcImpl(globalObject, abcState(globalObject), self);
    if (!impl)
        return { };
    impl->set(vm, AbcDataCache, { });
    impl->set(vm, AbcDataNegativeCache, { });
    return jsUndefined();
}

// As a set of weak references, which is what it is in CPython.
static JSValue abcDump(JSGlobalObject* globalObject, PyRecord* impl, AbcDataField field)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PySet* result = PySet::create(globalObject);
    MarkedArgumentBuffer classes;
    abcTakeSnapshot(impl, field, classes);
    for (unsigned i = 0; i < classes.size(); ++i) {
        PyWeakRef* reference = makeWeakRef(globalObject, classes.at(i));
        RETURN_IF_EXCEPTION(scope, { });
        result->add(globalObject, reference);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue abcGetDump(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* impl = abcImpl(globalObject, abcState(globalObject), self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue registry = abcDump(globalObject, impl, AbcDataRegistry);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue cache = abcDump(globalObject, impl, AbcDataCache);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue negativeCache = abcDump(globalObject, impl, AbcDataNegativeCache);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { registry, cache, negativeCache, makeInt(globalObject, impl->number) });
}

static JSValue abcGetCacheToken(JSGlobalObject* globalObject) { return makeInt(globalObject, abcState(globalObject)->number); }

static constexpr NativeDef abcFunctions[] = {
    PY_FUNCTION("get_cache_token", abcGetCacheToken, "()"),
    PY_FUNCTION("_abc_init", abcInit, "(self, /)"),
    PY_FUNCTION("_reset_registry", abcResetRegistry, "(self, /)"),
    PY_FUNCTION("_reset_caches", abcResetCaches, "(self, /)"),
    PY_FUNCTION("_get_dump", abcGetDump, "(self, /)"),
    PY_FUNCTION("_abc_register", abcRegister, "(self, subclass, /)"),
    PY_FUNCTION("_abc_instancecheck", abcInstanceCheck, "(self, instance, /)"),
    PY_FUNCTION("_abc_subclasscheck", abcSubclassCheck, "(self, subclass, /)"),
};

void initializeAbcModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->add(globalObject, "__doc__"_s, makeStr(getVM(globalObject), "Module contains faster C implementation of abc.ABCMeta"_s));
    module->addFunctions(globalObject, abcFunctions);
}

} // namespace Python
} // namespace JSC
