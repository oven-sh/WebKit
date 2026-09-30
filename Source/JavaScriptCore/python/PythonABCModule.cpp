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
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PyWeakReference.h"
#include "PythonIO.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"

// The module _abc: Modules/_abc.c of CPython. It is what abc.ABCMeta is written over.

namespace JSC { namespace Python {

namespace {

struct ABCModuleState final : NativeState {
    PYTHON_NATIVE_STATE(ABCModuleState);
    WriteBarrier<PyType> dataType;
    // It moves whenever a class is registered with any other, after which what was found not to be derived from something may be.
    uint64_t invalidationCounter { 0 };
};

template<typename Visitor> void ABCModuleState::visit(Visitor& visitor) { visitor.append(dataType); }

ABCModuleState& abcModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<ABCModuleState>(); }

// _abc_data: what an abstract base class keeps, as its _abc_impl. Each is a set of weak references, made when there is something to put in it.
struct ABCData final : NativeState {
    PYTHON_NATIVE_STATE(ABCData);
    WriteBarrier<PySet> registry;
    WriteBarrier<PySet> cache;
    WriteBarrier<PySet> negativeCache;
    uint64_t negativeCacheVersion { 0 };
};

template<typename Visitor>
void ABCData::visit(Visitor& visitor)
{
    visitor.append(registry);
    visitor.append(cache);
    visitor.append(negativeCache);
}

// abc_data_new()
PyStateObject* newData(JSGlobalObject* globalObject, PyType* type)
{
    auto* object = PyStateObject::create(globalObject->vm(), type->instanceStructure(), makeUnique<ABCData>());
    object->state<ABCData>().negativeCacheVersion = abcModuleState(globalObject).invalidationCounter;
    return object;
}

// _get_impl(). Null if it raised.
PyStateObject* implementationOf(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue implementation = getAttribute(globalObject, self, Identifier::fromString(vm, "_abc_impl"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!tryStateOf<ABCData>(implementation) || typeOf(globalObject, implementation) != abcModuleState(globalObject).dataType.get()) {
        raiseTypeError(globalObject, scope, "_abc_impl is set to a wrong type"_s);
        return nullptr;
    }
    return uncheckedDowncast<PyStateObject>(implementation.asCell());
}

// _in_weak_set(). Nothing if it raised.
std::optional<bool> isInWeakSet(JSGlobalObject* globalObject, const WriteBarrier<PySet>& set, JSValue object)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!set || !set->size())
        return false;
    JSValue reference = newWeakReference(globalObject, object);
    if (scope.exception()) [[unlikely]] {
        // There is no referring weakly to it, so it was never put there.
        if (catchException(globalObject, BuiltinType::TypeError))
            return false;
        return std::nullopt;
    }
    int found = set->find(globalObject, reference);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return found != PyHashTable::notFound;
}

} // anonymous namespace

// _destroy(): what a weak reference in one of the sets calls when what it refers to has gone. It takes the reference out of the set, if the set is still there. Its __self__ is a weak reference to the set.
PYTHON_NATIVE(abcDestroy)
{
    NATIVE_PROLOGUE();
    auto* toSet = dynamicDowncast<PyWeakReference>(uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee())->owner());
    JSObject* set = toSet ? toSet->referent() : nullptr;
    if (!set)
        RETURN_NONE();
    uncheckedDowncast<PySet>(set)->remove(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

namespace {

// _add_to_weak_set(). False if it raised.
bool addToWeakSet(JSGlobalObject* globalObject, JSCell* owner, WriteBarrier<PySet>& set, JSValue object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!set)
        set.set(vm, owner, PySet::create(globalObject));
    JSValue toSet = newWeakReference(globalObject, set.get());
    RETURN_IF_EXCEPTION(scope, false);
    auto* destroy = PyNativeFunction::create(vm, globalObject, 0, "_destroy"_s, abcDestroy, PyNativeFunction::Kind::Function, asObject(toSet), 0, ImplementationVisibility::Public, "($module, object, /)"_s);
    JSValue reference = newWeakReference(globalObject, object, destroy);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, set->add(globalObject, reference));
}

// PySet_New(), of one of the sets, which may not have been made
PySet* copyOf(JSGlobalObject* globalObject, const WriteBarrier<PySet>& set, BuiltinType type = BuiltinType::Set)
{
    VM& vm = globalObject->vm();
    PySet* copy = PySet::create(vm, globalObject->pyRealm()->structureFor(type));
    if (set)
        copy->copyFrom(vm, globalObject, *set.get());
    return copy;
}

// compute_abstract_methods(): the names of what is abstract in a class, which is its __abstractmethods__. False if it raised.
bool computeAbstractMethods(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PySet* abstracts = PySet::create(vm, globalObject->pyRealm()->structureFor(BuiltinType::FrozenSet));

    // What it has of its own
    JSValue contents = getAttribute(globalObject, self, names.dunder_dict);
    RETURN_IF_EXCEPTION(scope, false);
    // A list of them, since asking one whether it is abstract can change what there is.
    JSValue itemsView = callMethodNamed(globalObject, contents, Identifier::fromString(vm, "items"_s));
    RETURN_IF_EXCEPTION(scope, false);
    MarkedArgumentBuffer items;
    collect(globalObject, itemsView, items);
    RETURN_IF_EXCEPTION(scope, false);
    for (size_t i = 0; i < items.size(); ++i) {
        MarkedArgumentBuffer pair;
        if (!isList(items.at(i)) && !isTuple(items.at(i)) && !typeOf(globalObject, items.at(i))->lookup(vm, names.dunder_iter) && !typeOf(globalObject, items.at(i))->lookup(vm, names.dunder_getitem)) {
            raiseTypeError(globalObject, scope, "items() returned non-iterable"_s);
            return false;
        }
        collect(globalObject, items.at(i), pair);
        RETURN_IF_EXCEPTION(scope, false);
        if (pair.size() != 2) {
            raiseTypeError(globalObject, scope, "items() returned item which size is not 2"_s);
            return false;
        }
        bool isAbstractItem = isAbstract(globalObject, pair.at(1));
        RETURN_IF_EXCEPTION(scope, false);
        if (isAbstractItem) {
            abstracts->add(globalObject, pair.at(0));
            RETURN_IF_EXCEPTION(scope, false);
        }
    }

    // What it comes by, and has not put something else in the place of
    JSValue bases = getAttribute(globalObject, self, names.dunder_bases);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isTuple(bases)) {
        raiseTypeError(globalObject, scope, "__bases__ is not tuple"_s);
        return false;
    }
    for (unsigned i = 0; i < asTuple(bases)->length(); ++i) {
        JSValue inherited = getAttributeIfPresent(globalObject, asTuple(bases)->at(i), names.dunder_abstractmethods);
        RETURN_IF_EXCEPTION(scope, false);
        if (!inherited)
            continue;
        JSValue iterator = getIterator(globalObject, inherited);
        RETURN_IF_EXCEPTION(scope, false);
        for (;;) {
            JSValue key = iteratorNext(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, false);
            if (!key)
                break;
            JSString* name = stringIn(key);
            if (!name) {
                raiseTypeError(globalObject, scope, concatenate("attribute name must be string, not '"_s, typeName(globalObject, key), '\''));
                return false;
            }
            Identifier identifier = name->toIdentifier(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            JSValue value = getAttributeIfPresent(globalObject, self, identifier);
            RETURN_IF_EXCEPTION(scope, false);
            if (!value)
                continue;
            bool isAbstractItem = isAbstract(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            if (isAbstractItem) {
                abstracts->add(globalObject, key);
                RETURN_IF_EXCEPTION(scope, false);
            }
        }
    }
    scope.release();
    setAttribute(globalObject, self, names.dunder_abstractmethods, abstracts);
    return !scope.exception();
}

constexpr unsigned collectionFlags = PyType::IsSequence | PyType::IsMapping;

// _PyType_SetFlagsRecursive(): of a class and of all that are derived from it, but for what cannot be changed, and what is so already, and what is derived from those.
void setCollectionFlagRecursively(PyType* type, PyType::Flag flag)
{
    if (type->isImmutable() || (type->hasFlag(flag) && !type->hasFlag(flag == PyType::IsSequence ? PyType::IsMapping : PyType::IsSequence)))
        return;
    type->clearFlag(PyType::IsSequence);
    type->clearFlag(PyType::IsMapping);
    type->setFlag(flag);
    for (PyType* subclass : type->subclasses())
        setCollectionFlagRecursively(subclass, flag);
}

// subclasscheck_check_registry(): whether a class is derived from one that has been registered. Nothing if it raised.
std::optional<bool> isDerivedFromRegistered(JSGlobalObject* globalObject, PyStateObject* implementation, JSValue subclass)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& data = implementation->state<ABCData>();
    auto isRegistered = isInWeakSet(globalObject, data.registry, subclass);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*isRegistered)
        return true;
    if (!data.registry)
        return false;
    // A copy, since what is asked of each can register another.
    PySet* registry = copyOf(globalObject, data.registry, BuiltinType::FrozenSet);
    for (unsigned entry = registry->firstEntry(); entry < registry->entryCount(); ++entry) {
        JSValue key = registry->keyAt(entry);
        if (!key)
            continue;
        auto* reference = dynamicDowncast<PyWeakReference>(key);
        if (!reference) {
            raiseTypeError(globalObject, scope, "expected a weakref"_s);
            return std::nullopt;
        }
        JSObject* registered = reference->referent();
        if (!registered)
            continue;
        bool isDerived = isSubclassOf(globalObject, subclass, registered);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (isDerived) {
            addToWeakSet(globalObject, implementation, data.cache, subclass);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            return true;
        }
    }
    return false;
}

} // anonymous namespace

PYTHON_NATIVE(abcDataNew)
{
    return JSValue::encode(newData(globalObject, asType(callFrame->uncheckedArgument(0))));
}

// _reset_registry(self, /)
PYTHON_NATIVE(abcResetRegistry)
{
    NATIVE_PROLOGUE();
    auto* implementation = implementationOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto& registry = implementation->state<ABCData>().registry)
        registry->clear(vm);
    RETURN_NONE();
}

// _reset_caches(self, /)
PYTHON_NATIVE(abcResetCaches)
{
    NATIVE_PROLOGUE();
    auto* implementation = implementationOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto& data = implementation->state<ABCData>();
    if (data.cache)
        data.cache->clear(vm);
    if (data.negativeCache)
        data.negativeCache->clear(vm);
    RETURN_NONE();
}

// _get_dump(self, /)
PYTHON_NATIVE(abcGetDump)
{
    NATIVE_PROLOGUE();
    auto* implementation = implementationOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto& data = implementation->state<ABCData>();
    return JSValue::encode(PyTuple::create(globalObject, { copyOf(globalObject, data.registry), copyOf(globalObject, data.cache), copyOf(globalObject, data.negativeCache), intFromUInt64(globalObject, data.negativeCacheVersion) }));
}

// _abc_init(self, /)
PYTHON_NATIVE(abcInit)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    computeAbstractMethods(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    setAttribute(globalObject, self, Identifier::fromString(vm, "_abc_impl"_s), newData(globalObject, abcModuleState(globalObject).dataType.get()));
    RETURN_IF_EXCEPTION(scope, { });
    // collections.abc.Sequence and collections.abc.Mapping say in this way that a sequence pattern, or a mapping pattern, matches what is derived from them.
    if (!isClass(self))
        RETURN_NONE();
    PyType* type = asType(self);
    Identifier name = Identifier::fromString(vm, "__abc_tpflags__"_s);
    JSValue flags = type->getDirect(vm, name);
    if (!flags)
        RETURN_NONE();
    JSCell::deleteProperty(type, globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (typeOf(globalObject, flags) != realm->typeInt())
        RETURN_NONE();
    auto value = toCLong(globalObject, flags);
    RETURN_IF_EXCEPTION(scope, { });
    if ((*value & (PyType::cpythonSequence | PyType::cpythonMapping)) == (PyType::cpythonSequence | PyType::cpythonMapping))
        return JSValue::encode(raiseTypeError(globalObject, scope, "__abc_tpflags__ cannot be both Py_TPFLAGS_SEQUENCE and Py_TPFLAGS_MAPPING"_s));
    if (*value & PyType::cpythonSequence)
        type->setFlag(PyType::IsSequence);
    if (*value & PyType::cpythonMapping)
        type->setFlag(PyType::IsMapping);
    RETURN_NONE();
}

// _abc_register(self, subclass, /)
PYTHON_NATIVE(abcRegister)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue subclass = args[1];
    if (!isClass(subclass))
        return JSValue::encode(raiseTypeError(globalObject, scope, "Can only register classes"_s));
    bool isAlready = isSubclassOf(globalObject, subclass, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isAlready)
        return JSValue::encode(subclass);
    // Not until now, so that X.register(X) is allowed, and does nothing.
    bool wouldGoRound = isSubclassOf(globalObject, self, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    if (wouldGoRound)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "Refusing to create an inheritance cycle"_s));
    auto* implementation = implementationOf(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    addToWeakSet(globalObject, implementation, implementation->state<ABCData>().registry, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    ++abcModuleState(globalObject).invalidationCounter;
    if (isClass(self)) {
        if (asType(self)->hasFlag(PyType::IsSequence))
            setCollectionFlagRecursively(asType(subclass), PyType::IsSequence);
        else if (asType(self)->hasFlag(PyType::IsMapping))
            setCollectionFlagRecursively(asType(subclass), PyType::IsMapping);
    }
    return JSValue::encode(subclass);
}

// _abc_instancecheck(self, instance, /)
PYTHON_NATIVE(abcInstanceCheck)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue instance = args[1];
    auto* implementation = implementationOf(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    auto& data = implementation->state<ABCData>();
    JSValue subclass = getAttribute(globalObject, instance, names.dunder_class);
    RETURN_IF_EXCEPTION(scope, { });
    auto isCached = isInWeakSet(globalObject, data.cache, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    if (*isCached)
        return JSValue::encode(jsBoolean(true));
    JSValue subtype = typeOf(globalObject, instance)->object();
    if (subtype == subclass) {
        if (data.negativeCacheVersion == abcModuleState(globalObject).invalidationCounter) {
            auto isKnownNotToBe = isInWeakSet(globalObject, data.negativeCache, subclass);
            RETURN_IF_EXCEPTION(scope, { });
            if (*isKnownNotToBe)
                return JSValue::encode(jsBoolean(false));
        }
        RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, self, names.dunder_subclasscheck, subclass)));
    }
    // It says that it is of one class and is of another. Either will do.
    JSValue result = callMethodNamed(globalObject, self, names.dunder_subclasscheck, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    bool isSo = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (isSo)
        return JSValue::encode(result);
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, self, names.dunder_subclasscheck, subtype)));
}

// _abc_subclasscheck(self, subclass, /)
PYTHON_NATIVE(abcSubclassCheck)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue subclass = args[1];
    if (!isClass(subclass))
        return JSValue::encode(raiseTypeError(globalObject, scope, "issubclass() arg 1 must be a class"_s));
    auto* implementation = implementationOf(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    auto& data = implementation->state<ABCData>();
    auto remember = [&] (WriteBarrier<PySet>& set, bool answer) {
        addToWeakSet(globalObject, implementation, set, subclass);
        RETURN_IF_EXCEPTION(scope, EncodedJSValue());
        return JSValue::encode(jsBoolean(answer));
    };

    // 1. What has been found to be
    auto isCached = isInWeakSet(globalObject, data.cache, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    if (*isCached)
        return JSValue::encode(jsBoolean(true));

    // 2. What has been found not to be, unless something has been registered since
    uint64_t invalidationCounter = abcModuleState(globalObject).invalidationCounter;
    if (data.negativeCacheVersion < invalidationCounter) {
        if (data.negativeCache)
            data.negativeCache->clear(vm);
        data.negativeCacheVersion = invalidationCounter;
    } else {
        auto isKnownNotToBe = isInWeakSet(globalObject, data.negativeCache, subclass);
        RETURN_IF_EXCEPTION(scope, { });
        if (*isKnownNotToBe)
            return JSValue::encode(jsBoolean(false));
    }

    // 3. What the class itself has to say
    JSValue ok = callMethodNamed(globalObject, self, names.dunder_subclasshook, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    if (ok == jsBoolean(true))
        return remember(data.cache, true);
    if (ok == jsBoolean(false))
        return remember(data.negativeCache, false);
    if (ok != realm->notImplemented())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::AssertionError, "__subclasshook__ must return either False, True, or NotImplemented"_s));

    // 4. Whether it is derived from it
    if (isClass(self) && asType(subclass)->isSubtypeOf(asType(self)))
        return remember(data.cache, true);

    // 5. Or from what has been registered with it
    auto isRegistered = isDerivedFromRegistered(globalObject, implementation, subclass);
    RETURN_IF_EXCEPTION(scope, { });
    if (*isRegistered)
        return JSValue::encode(jsBoolean(true));

    // 6. Or counts as derived from what is derived from it
    JSValue subclasses = callMethodNamed(globalObject, self, Identifier::fromString(vm, "__subclasses__"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = tryList(subclasses);
    if (!list)
        return JSValue::encode(raiseTypeError(globalObject, scope, "__subclasses__() must return a list"_s));
    for (unsigned i = 0; i < list->length(); ++i) {
        bool isDerived = isSubclassOf(globalObject, subclass, list->getIndexQuickly(i));
        RETURN_IF_EXCEPTION(scope, { });
        if (isDerived)
            return remember(data.cache, true);
    }
    return remember(data.negativeCache, false);
}

PYTHON_NATIVE(abcGetCacheToken)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(intFromUInt64(globalObject, abcModuleState(globalObject).invalidationCounter));
}

JSObject* createABCModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = abcModuleState(globalObject);
    if (!state.dataType) {
        PyType* type = createBuiltinType(globalObject, "_abc._abc_data"_s, realm->typeObject(), PyType::Layout::Native, 0);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.dataType.set(vm, realm, type);
        addMethods(globalObject, type, { { "__new__"_s, abcDataNew, PyNativeFunction::Kind::New, 0, "($type, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked } });
    }
    JSObject* module = newBuiltinModule(globalObject, "_abc"_s);
    addFunction(globalObject, module, "get_cache_token"_s, abcGetCacheToken);
    addFunction(globalObject, module, "_abc_init"_s, abcInit);
    addFunction(globalObject, module, "_reset_registry"_s, abcResetRegistry);
    addFunction(globalObject, module, "_reset_caches"_s, abcResetCaches);
    addFunction(globalObject, module, "_get_dump"_s, abcGetDump);
    addFunction(globalObject, module, "_abc_register"_s, abcRegister);
    addFunction(globalObject, module, "_abc_instancecheck"_s, abcInstanceCheck);
    addFunction(globalObject, module, "_abc_subclasscheck"_s, abcSubclassCheck);
    return module;
}

} } // namespace JSC::Python
