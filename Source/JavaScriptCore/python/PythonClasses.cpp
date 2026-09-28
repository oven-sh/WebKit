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
#include "JSLexicalEnvironment.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "PythonSequences.h"

// Making classes, and asking what is an instance of what.

namespace JSC { namespace Python {

// ---- The order in which the bases of a class are searched

// C3: a class comes before its bases, and they in the order they are given, for every class in the hierarchy at once.
static PyTuple* linearize(JSGlobalObject* globalObject, PyTuple* bases)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    for (unsigned i = 0; i < bases->length(); ++i) {
        for (unsigned j = i + 1; j < bases->length(); ++j) {
            if (asType(bases->at(i)) == asType(bases->at(j))) {
                raiseTypeError(globalObject, scope, makeString("duplicate base class "_s, asType(bases->at(i))->nameWithoutModule(globalObject)));
                return nullptr;
            }
        }
    }

    Vector<Vector<PyType*, 8>, 4> sequences;
    for (auto& base : bases->span()) {
        Vector<PyType*, 8> order;
        for (auto& entry : asType(base.get())->mro()->span())
            order.append(asType(entry.get()));
        sequences.append(WTF::move(order));
    }
    Vector<PyType*, 8> given;
    for (auto& base : bases->span())
        given.append(asType(base.get()));
    sequences.append(WTF::move(given));

    Vector<size_t, 4> positions(sequences.size(), [] (size_t) -> size_t { return 0; });
    Vector<PyType*, 16> result;
    while (true) {
        bool isDone = true;
        PyType* next = nullptr;
        for (size_t i = 0; i < sequences.size() && !next; ++i) {
            if (positions[i] >= sequences[i].size())
                continue;
            isDone = false;
            // The first of a sequence, if it is not further on in another.
            PyType* candidate = sequences[i][positions[i]];
            bool isBlocked = false;
            for (size_t j = 0; j < sequences.size() && !isBlocked; ++j) {
                for (size_t k = positions[j] + 1; k < sequences[j].size(); ++k)
                    isBlocked |= sequences[j][k] == candidate;
            }
            if (!isBlocked)
                next = candidate;
        }
        if (isDone)
            break;
        if (!next) {
            StringBuilder names;
            bool isFirst = true;
            Vector<PyType*, 8> named;
            for (size_t i = 0; i < sequences.size(); ++i) {
                if (positions[i] >= sequences[i].size() || named.contains(sequences[i][positions[i]]))
                    continue;
                named.append(sequences[i][positions[i]]);
                String name = sequences[i][positions[i]]->nameString(globalObject);
                if (!isFirst)
                    names.append(", "_s);
                isFirst = false;
                names.append(name);
            }
            raiseTypeError(globalObject, scope, makeString("Cannot create a consistent method resolution order (MRO) for bases "_s, names.toString()));
            return nullptr;
        }
        result.append(next);
        for (size_t i = 0; i < sequences.size(); ++i) {
            if (positions[i] < sequences[i].size() && sequences[i][positions[i]] == next)
                ++positions[i];
        }
    }

    PyTuple* order = PyTuple::create(globalObject, result.size());
    for (unsigned i = 0; i < result.size(); ++i)
        order->initializeAt(vm, i, result[i]);
    return order;
}

// The nearest of a class and its bases whose instances are laid out differently from those of its own base: solid_base() of CPython's
// Objects/typeobject.c. A class with __slots__ is one, and one without is not.
static PyType* solidBase(PyType* type)
{
    if (!type->base())
        return type;
    PyType* base = solidBase(type->base());
    return type->basicSize() != base->basicSize() || type->itemSize() != base->itemSize() ? type : base;
}

// The class, and then what linearize() gives: what type.mro() returns.
PyTuple* defaultOrder(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* rest = linearize(globalObject, type->bases());
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyTuple* order = PyTuple::create(globalObject, rest->length() + 1);
    order->initializeAt(vm, 0, type);
    for (unsigned i = 0; i < rest->length(); ++i)
        order->initializeAt(vm, i + 1, rest->at(i));
    return order;
}

// The order for a class as it is now. A metaclass can have its own idea of it: mro_invoke() and mro_check() of the same.
static PyTuple* computeOrder(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (type->metatype() == globalObject->pyRealm()->typeType())
        RELEASE_AND_RETURN(scope, defaultOrder(globalObject, type));

    JSValue method = getAttribute(globalObject, type, Identifier::fromString(vm, "mro"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSValue result = call(globalObject, method);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyTuple* order = tupleFromIterable(globalObject, result);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!order->length()) {
        raiseTypeError(globalObject, scope, "type MRO must not be empty"_s);
        return nullptr;
    }
    PyType* solid = solidBase(type);
    for (auto& entry : order->span()) {
        if (!isClass(entry.get())) {
            raiseTypeError(globalObject, scope, makeString("mro() returned a non-class ('"_s, typeName(globalObject, entry.get()), "')"_s));
            return nullptr;
        }
        if (!solid->isSubtypeOf(solidBase(asType(entry.get())))) {
            raiseTypeError(globalObject, scope, makeString("mro() returned base with unsuitable layout ('"_s, asType(entry.get())->nameString(globalObject), "')"_s));
            return nullptr;
        }
    }
    // What is kept is the data of each class, whichever language made it.
    PyTuple* types = PyTuple::create(globalObject, order->length());
    for (unsigned i = 0; i < order->length(); ++i)
        types->initializeAt(vm, i, asType(order->at(i)));
    return types;
}

// The base whose instances are laid out as the new class's will be. All the others have to be content with that: best_base() of the same.
static PyType* bestBase(JSGlobalObject* globalObject, PyTuple* bases)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* best = nullptr;
    PyType* winner = nullptr;
    for (auto& entry : bases->span()) {
        if (!isClass(entry.get())) {
            // list[int] can be derived from, by a class statement, which asks it what to derive from in its place.
            bool standsForOthers = !!getAttributeIfPresent(globalObject, entry.get(), vm.pythonNames().dunder_mro_entries);
            RETURN_IF_EXCEPTION(scope, nullptr);
            raiseTypeError(globalObject, scope, standsForOthers ? "type() doesn't support MRO entry resolution; use types.new_class()"_s : "bases must be types"_s);
            return nullptr;
        }
        auto* base = asType(entry.get());
        if (!base->hasFlag(PyType::IsBaseType)) {
            raiseTypeError(globalObject, scope, makeString("type '"_s, base->nameString(globalObject), "' is not an acceptable base type"_s));
            return nullptr;
        }
        PyType* candidate = solidBase(base);
        if (!winner) {
            winner = candidate;
            best = base;
        } else if (winner->isSubtypeOf(candidate))
            continue;
        else if (candidate->isSubtypeOf(winner)) {
            winner = candidate;
            best = base;
        } else {
            raiseTypeError(globalObject, scope, "multiple bases have instance lay-out conflict"_s);
            return nullptr;
        }
    }
    return best;
}

// ---- type.__new__

// What a name like __x is inside a class: _Py_Mangle()
static String mangle(const String& className, const String& name)
{
    if (!name.startsWith("__"_s) || name.endsWith("__"_s) || name.contains('.'))
        return name;
    unsigned underscores = 0;
    while (underscores < className.length() && className[underscores] == '_')
        ++underscores;
    if (underscores == className.length())
        return name;
    return makeString('_', StringView(className).substring(underscores), name);
}

void addInstanceDescriptors(JSGlobalObject* globalObject, PyType* type, bool addsDict, bool addsWeakReferences)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    if (addsDict)
        type->putDirect(vm, names.dunder_dict, PyGetSetDescriptor::create(globalObject, type, "__dict__"_s, getInstanceDict, setInstanceDict, false, "dictionary for instance variables"_s));
    if (addsWeakReferences)
        type->putDirect(vm, names.dunder_weakref, PyGetSetDescriptor::create(globalObject, type, "__weakref__"_s, getWeakReferences, nullptr, false, "list of weak references to the object"_s));
}

static bool callSetNames(JSGlobalObject* globalObject, PyType* type, PyDict* namespaceDict)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    namespaceDict->forEach(globalObject, [&] (JSValue key, JSValue value) {
        if (!value.isObject())
            return true;
        JSValue self;
        JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_set_name, self);
        RETURN_IF_EXCEPTION(scope, false);
        if (!method)
            return true;
        callMethod(globalObject, method, self, type, key);
        if (scope.exception()) [[unlikely]] {
            addNoteToRaised(globalObject, [&] {
                return makeString("Error calling __set_name__ on '"_s, typeName(globalObject, value), "' instance "_s, repr(globalObject, key), " in '"_s, type->nameString(globalObject), '\'');
            });
            return false;
        }
        return true;
    });
    return !scope.exception();
}

static JSValue callWithKeywordDict(JSGlobalObject* globalObject, JSValue callable, MarkedArgumentBuffer& arguments, PyDict* keywords)
{
    VM& vm = globalObject->vm();
    if (!keywords || !keywords->size())
        return call(globalObject, callable, arguments);
    KeywordNames* names = KeywordNames::create(vm, CopyOnWriteArrayWithContiguous, keywords->size());
    unsigned i = 0;
    keywords->forEach(globalObject, [&] (JSValue key, JSValue value) {
        names->setIndex(vm, i++, key);
        arguments.append(value);
        return true;
    });
    return callWithKeywords(globalObject, callable, arguments, names);
}

JSValue newType(JSGlobalObject* globalObject, PyType* metatype, JSString* name, PyTuple* bases, PyDict* namespaceDict, PyDict* keywords)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();

    if (!bases->length())
        bases = PyTuple::create(globalObject, { realm->typeObject() });
    PyType* base = bestBase(globalObject, bases);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* order = linearize(globalObject, bases);
    RETURN_IF_EXCEPTION(scope, { });

    // What follows is type_new_impl() of CPython's Objects/typeobject.c and what that calls, in the same order, since the order is that of the __dict__.

    // ---- __slots__: which attributes instances have room for, and whether they have a __dict__ and can be weakly referred to besides
    bool mayAddDict = !base->hasFlag(PyType::HasInstanceDict);
    bool mayAddWeakReferences = !base->hasFlag(PyType::HasWeakReferences) && !base->itemSize();
    bool addsDict = false;
    bool addsWeakReferences = false;
    Vector<String> slots;
    JSValue slotsValue = namespaceDict->getString(globalObject, "__slots__"_s);
    if (!slotsValue) {
        addsDict = mayAddDict;
        addsWeakReferences = mayAddWeakReferences;
    } else {
        MarkedArgumentBuffer given;
        if (slotsValue.isString())
            given.append(slotsValue);
        else {
            collect(globalObject, slotsValue, given);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (given.size() && base->itemSize())
            return raiseTypeError(globalObject, scope, makeString("nonempty __slots__ not supported for subtype of '"_s, base->nameString(globalObject), '\''));
        for (unsigned i = 0; i < given.size(); ++i) {
            if (!given.at(i).isString())
                return raiseTypeError(globalObject, scope, makeString("__slots__ items must be strings, not '"_s, typeName(globalObject, given.at(i)), '\''));
            String slot = asString(given.at(i))->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isIdentifier(slot))
                return raiseTypeError(globalObject, scope, "__slots__ must be identifiers"_s);
            if (slot == "__dict__"_s) {
                if (!mayAddDict || addsDict)
                    return raiseTypeError(globalObject, scope, "__dict__ slot disallowed: we already got one"_s);
                addsDict = true;
            }
            if (slot == "__weakref__"_s) {
                if (!mayAddWeakReferences || addsWeakReferences)
                    return raiseTypeError(globalObject, scope, "__weakref__ slot disallowed: we already got one"_s);
                addsWeakReferences = true;
            }
        }
        String className = name->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        for (unsigned i = 0; i < given.size(); ++i) {
            String slot = asString(given.at(i))->value(globalObject);
            if (slot == "__dict__"_s || slot == "__weakref__"_s)
                continue;
            slot = mangle(className, slot);
            // These three are put in the namespace for the sake of making the class, and taken out again below.
            if (namespaceDict->getString(globalObject, slot) && slot != "__qualname__"_s && slot != "__classcell__"_s && slot != "__classdictcell__"_s)
                return raiseValueError(globalObject, scope, makeString(reprOfString(slot), " in __slots__ conflicts with class variable"_s));
            slots.append(slot);
        }
        std::ranges::sort(slots, [] (const String& a, const String& b) { return codePointCompareLessThan(a, b); });
        // The other bases may provide what the first does not.
        for (auto& entry : bases->span()) {
            PyType* other = asType(entry.get());
            if (other == base)
                continue;
            addsDict |= mayAddDict && other->hasFlag(PyType::HasInstanceDict);
            addsWeakReferences |= mayAddWeakReferences && other->hasFlag(PyType::HasWeakReferences);
        }
    }

    PyType* type = PyType::create(vm, globalObject, metatype, name, bases, base, order);
    type->addToLayout(slots.size(), addsDict, addsWeakReferences);

    // ---- What is in the namespace
    type->putDirect(vm, names.private_qualname, name);
    namespaceDict->forEach(globalObject, [&] (JSValue key, JSValue value) {
        if (!key.isString()) {
            // No attribute, since there is no naming it, but it is in the __dict__.
            PyDict::backedBy(globalObject, type)->set(globalObject, key, value);
            return !scope.exception();
        }
        auto property = asString(key)->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (property == names.dunder_qualname) {
            if (!value.isString()) {
                raiseTypeError(globalObject, scope, makeString("type __qualname__ must be a str, not "_s, typeName(globalObject, value)));
                return false;
            }
            type->putDirect(vm, names.private_qualname, value);
            return true;
        }

        // These are what they are whether or not they are decorated as such.
        bool isPlainFunction = value.isCell() && value.asCell()->type() == JSFunctionType && !value.asCell()->inherits<PyNativeFunction>();
        if (isPlainFunction && property == names.dunder_new)
            value = PyNativeObject::create(globalObject, BuiltinType::StaticMethod, value);
        else if (isPlainFunction && (property == names.dunder_init_subclass || property == names.dunder_class_getitem))
            value = PyNativeObject::create(globalObject, BuiltinType::ClassMethod, value);
        putStoredAttribute(vm, type, property, value);
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });

    // The module is that of whoever is making the class, if the namespace does not say.
    if (!type->lookupOwn(vm, names.dunder_module)) {
        if (JSObject* globals = globalsOfCaller(globalObject)) {
            if (JSValue moduleName = getStoredAttribute(vm, globals, names.dunder_name))
                type->putDirect(vm, names.dunder_module, moduleName);
        }
    }

    // ---- What the class provides for its instances
    if (slotsValue) {
        // As they were made out to be, which is not always as __slots__ has them. Two classes are laid out alike only if these are the same.
        PyTuple* names = PyTuple::create(globalObject, slots.size());
        for (unsigned i = 0; i < slots.size(); ++i)
            names->initializeAt(vm, i, jsString(vm, slots[i]));
        type->putDirect(vm, vm.pythonNames().private_slots, names);
    }
    unsigned slotOffset = base->basicSize();
    for (auto& slot : slots) {
        type->putDirect(vm, Identifier::fromString(vm, slot), createMemberDescriptor(globalObject, type, jsString(vm, slot), &names.slotStorage(slotOffset)));
        slotOffset += sizeof(void*);
    }
    addInstanceDescriptors(globalObject, type, addsDict, addsWeakReferences);

    if (!type->lookupOwn(vm, names.dunder_doc))
        type->putDirect(vm, names.dunder_doc, jsUndefined());
    // What says when two of them are equal, and not what their hash is, cannot be hashed.
    if (type->lookupOwn(vm, names.dunder_eq) && !type->lookupOwn(vm, names.dunder_hash))
        type->putDirect(vm, names.dunder_hash, jsUndefined());
    if (metatype != realm->typeType()) {
        PyTuple* ownOrder = computeOrder(globalObject, type);
        RETURN_IF_EXCEPTION(scope, { });
        type->setOrder(vm, ownOrder);
    }
    callSetNames(globalObject, type, namespaceDict);
    RETURN_IF_EXCEPTION(scope, { });

    // super().__init_subclass__(**keywords)
    if (std::ranges::none_of(type->mro()->span(), [&] (auto& entry) { return entry.get() == type; })) {
        // Only an mro() that leaves the class out of its own order can bring this about.
        String typeName = type->nameString(globalObject);
        return raiseTypeError(globalObject, scope, makeString("super(type, obj): obj (type "_s, typeName, ") is not an instance or subtype of type ("_s, typeName, ")."_s));
    }
    JSValue hook = type->lookupAfter(vm, type, names.dunder_init_subclass);
    if (hook) {
        JSValue bound = bindDescriptor(globalObject, hook, JSValue(), type);
        RETURN_IF_EXCEPTION(scope, { });
        MarkedArgumentBuffer arguments;
        callWithKeywordDict(globalObject, bound, arguments, keywords);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return type;
}

// The end of newType(), for a class that JavaScript made.
static void javaScriptClassWasDefined(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* prototype = type->javaScriptPrototype();
    PropertyNameArrayBuilder properties(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    prototype->methodTable()->getOwnPropertyNames(prototype, globalObject, properties, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto& property : properties) {
        JSValue value = prototype->getDirect(vm, property);
        if (!value || !value.isObject())
            continue;
        JSValue self;
        JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_set_name, self);
        RETURN_IF_EXCEPTION(scope, void());
        if (!method)
            continue;
        callMethod(globalObject, method, self, type->object(), jsString(vm, property.string()));
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (JSValue hook = type->lookupAfter(vm, type, vm.pythonNames().dunder_init_subclass)) {
        JSValue bound = bindDescriptor(globalObject, hook, JSValue(), type);
        RETURN_IF_EXCEPTION(scope, void());
        scope.release();
        call(globalObject, bound);
    }
}

// ---- The class statement

// A base that is not a class may say which classes to derive from in its place: Generic[T] does.
static PyTuple* resolveBases(JSGlobalObject* globalObject, PyTuple* bases)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool changed = false;
    MarkedArgumentBuffer resolved;
    for (auto& entry : bases->span()) {
        JSValue base = entry.get();
        if (isClass(base)) {
            resolved.append(base);
            continue;
        }
        JSValue method = getAttributeIfPresent(globalObject, base, vm.pythonNames().dunder_mro_entries);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!method) {
            resolved.append(base);
            continue;
        }
        JSValue replacement = call(globalObject, method, bases);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!isTuple(replacement)) {
            raiseTypeError(globalObject, scope, "__mro_entries__ must return a tuple"_s);
            return nullptr;
        }
        for (auto& item : uncheckedDowncast<PyTuple>(replacement.asCell())->span())
            resolved.append(item.get());
        changed = true;
    }
    return changed ? PyTuple::createFromArguments(globalObject, resolved) : bases;
}

// The most derived of the metaclasses of the bases and the one that was asked for.
static PyType* calculateMetaclass(JSGlobalObject* globalObject, PyType* metatype, PyTuple* bases)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* winner = metatype;
    for (auto& entry : bases->span()) {
        PyType* candidate = typeOf(globalObject, entry.get());
        if (winner->isSubtypeOf(candidate))
            continue;
        if (candidate->isSubtypeOf(winner)) {
            winner = candidate;
            continue;
        }
        raiseTypeError(globalObject, scope, "metaclass conflict: the metaclass of a derived class must be a (non-strict) subclass of the metaclasses of all its bases"_s);
        return nullptr;
    }
    return winner;
}

JSValue buildClass(JSGlobalObject* globalObject, JSValue body, JSString* name, PyTuple* originalBases, PyDict* keywords)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();

    PyTuple* bases = resolveBases(globalObject, originalBases);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue metaclass;
    if (keywords) {
        metaclass = keywords->remove(globalObject, jsNontrivialString(vm, "metaclass"_s));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!metaclass)
        metaclass = bases->length() ? typeOf(globalObject, bases->at(0)) : realm->typeType();
    if (isClass(metaclass)) {
        metaclass = calculateMetaclass(globalObject, asType(metaclass), bases);
        RETURN_IF_EXCEPTION(scope, { });
    }

    JSValue namespaceValue;
    if (metaclass.asCell() != realm->typeType()) {
        JSValue prepare = getAttributeIfPresent(globalObject, metaclass, names.dunder_prepare);
        RETURN_IF_EXCEPTION(scope, { });
        if (prepare) {
            MarkedArgumentBuffer arguments;
            arguments.append(name);
            arguments.append(bases);
            namespaceValue = callWithKeywordDict(globalObject, prepare, arguments, keywords);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    if (!namespaceValue)
        namespaceValue = PyDict::create(globalObject);

    JSValue environment = call(globalObject, body, namespaceValue);
    RETURN_IF_EXCEPTION(scope, { });

    if (bases != originalBases) {
        setItem(globalObject, namespaceValue, jsNontrivialString(vm, "__orig_bases__"_s), originalBases);
        RETURN_IF_EXCEPTION(scope, { });
    }

    MarkedArgumentBuffer arguments;
    arguments.append(name);
    arguments.append(bases);
    arguments.append(namespaceValue);
    JSValue result = callWithKeywordDict(globalObject, metaclass, arguments, keywords);
    RETURN_IF_EXCEPTION(scope, { });

    // Now that there is a class, the methods that use super() or __class__ can be told which it is.
    if (auto* scopeObject = dynamicDowncast<JSLexicalEnvironment>(environment)) {
        SymbolTableEntry::Fast entry = scopeObject->symbolTable()->get(names.dunder_class.impl());
        if (!entry.isNull())
            scopeObject->variableAt(entry.scopeOffset()).set(vm, scopeObject, result);
        // And what looks in the class for names, as its annotations do, looks from now on in the class and not in what it was made from.
        entry = scopeObject->symbolTable()->get(names.dunder_classdict.impl());
        if (!entry.isNull() && isType(result)) {
            JSValue dict = getAttribute(globalObject, result, names.dunder_dict);
            RETURN_IF_EXCEPTION(scope, { });
            scopeObject->variableAt(entry.scopeOffset()).set(vm, scopeObject, dict);
        }
    }
    return result;
}

// ---- C.__bases__ = ...: type_set_bases() of CPython's Objects/typeobject.c

// Works out again the order for a class and for all that is derived from it, noting what each had. False if something has been raised.
static bool recomputeOrders(JSGlobalObject* globalObject, PyType* type, MarkedArgumentBuffer& changed)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* order = computeOrder(globalObject, type);
    RETURN_IF_EXCEPTION(scope, false);
    changed.append(type);
    changed.append(type->mro());
    type->setOrder(vm, order);
    for (PyType* subclass : type->subclasses()) {
        bool ok = recomputeOrders(globalObject, subclass, changed);
        RETURN_IF_EXCEPTION(scope, false);
        ASSERT_UNUSED(ok, ok);
    }
    return true;
}

void setBases(JSGlobalObject* globalObject, PyType* type, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = type->nameString(globalObject);
    if (!value) {
        raiseTypeError(globalObject, scope, makeString("cannot delete '__bases__' attribute of immutable type '"_s, name, '\''));
        return;
    }
    // FIXME: A class that JavaScript made is derived from what its prototype is derived from, and it is that that would have to be changed.
    if (type->javaScriptConstructor()) {
        raiseTypeError(globalObject, scope, makeString("cannot set '__bases__' attribute of immutable type '"_s, name, '\''));
        return;
    }
    if (!isTuple(value)) {
        raiseTypeError(globalObject, scope, makeString("can only assign tuple to "_s, name, ".__bases__, not "_s, typeName(globalObject, value)));
        return;
    }
    PyTuple* bases = asTuple(value);
    if (!bases->length()) {
        raiseTypeError(globalObject, scope, makeString("can only assign non-empty tuple to "_s, name, ".__bases__, not ()"_s));
        return;
    }
    for (auto& entry : bases->span()) {
        if (!isClass(entry.get())) {
            raiseTypeError(globalObject, scope, makeString(name, ".__bases__ must be tuple of classes, not '"_s, typeName(globalObject, entry.get()), '\''));
            return;
        }
        if (asType(entry.get())->isSubtypeOf(type)) {
            raiseTypeError(globalObject, scope, "a __bases__ item causes an inheritance cycle"_s);
            return;
        }
    }
    PyType* base = bestBase(globalObject, bases);
    RETURN_IF_EXCEPTION(scope, void());
    PyType* oldBase = type->base();
    // In CPython what is collected is freed in one way and what is not in another, and it is by that that they are told apart first.
    constexpr unsigned long isCollected = 1ul << 14;
    if ((base->flagsForPython() & isCollected) != (oldBase->flagsForPython() & isCollected)) {
        raiseTypeError(globalObject, scope, makeString("__bases__ assignment: '"_s, base->nameString(globalObject), "' deallocator differs from '"_s, oldBase->nameString(globalObject), '\''));
        return;
    }
    if (!areLaidOutAlike(globalObject, oldBase, base)) {
        raiseTypeError(globalObject, scope, makeString("__bases__ assignment: '"_s, base->nameString(globalObject), "' object layout differs from '"_s, oldBase->nameString(globalObject), '\''));
        return;
    }

    PyTuple* oldBases = type->bases();
    type->setBases(vm, bases, base);
    MarkedArgumentBuffer changed;
    recomputeOrders(globalObject, type, changed);
    if (!scope.exception())
        return;
    // As it was.
    for (size_t i = changed.size(); i; i -= 2)
        asType(changed.at(i - 2))->setOrder(vm, asTuple(changed.at(i - 1)));
    type->setBases(vm, oldBases, oldBase);
}

// ---- super

// Whether super(type, object) makes sense, and the class whose order is to be searched: supercheck() of CPython's Objects/typeobject.c.
PyType* superCheck(JSGlobalObject* globalObject, PyType* type, JSValue object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A class derived from it, for a class method
    if (isClass(object) && asType(object)->isSubtypeOf(type))
        return asType(object);
    PyType* typeOfObject = typeOf(globalObject, object);
    if (typeOfObject->isSubtypeOf(type))
        return typeOfObject;
    // What stands in for an instance says so with __class__.
    JSValue claimed = getAttributeIfPresent(globalObject, object, vm.pythonNames().dunder_class);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (claimed && isClass(claimed) && asType(claimed) != typeOfObject && asType(claimed)->isSubtypeOf(type))
        return asType(claimed);
    raiseTypeError(globalObject, scope, makeString("super(type, obj): obj ("_s, isClass(object) ? "type "_s : "instance of "_s, (isClass(object) ? asType(object) : typeOfObject)->nameString(globalObject), ") is not an instance or subtype of type ("_s, type->nameString(globalObject), ")."_s));
    return nullptr;
}

JSValue getSuperAttribute(JSGlobalObject* globalObject, JSValue superObject, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto* object = uncheckedDowncast<PyNativeObject>(superObject.asCell());
    JSValue after = object->field(0);
    JSValue instance = object->field(1);
    JSValue startType = object->field(2);
    // super().__class__ is super.
    if (!startType || !isClass(startType) || name == vm.pythonNames().dunder_class)
        return { };
    auto* start = asType(startType);
    JSValue attribute = start->lookupAfter(vm, asType(after), name);
    if (!attribute)
        return { };
    // super(C, D) where D is a class: what is found is got from the class, and not from an instance.
    bool isForClass = isClass(instance) && asType(instance) == start;
    return bindDescriptor(globalObject, attribute, isForClass ? JSValue() : instance, start);
}

// ---- isinstance() and issubclass()

static bool checkClassInfo(JSGlobalObject* globalObject, JSValue value, JSValue classInfo, bool isInstanceCheck)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();

    // A union is as a tuple of what is in it.
    if (isUnion(globalObject, classInfo))
        classInfo = argumentsOfUnion(classInfo);

    if (isTuple(classInfo)) {
        for (auto& entry : uncheckedDowncast<PyTuple>(classInfo.asCell())->span()) {
            bool result = checkClassInfo(globalObject, value, entry.get(), isInstanceCheck);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return true;
        }
        return false;
    }

    if (isClass(classInfo)) {
        auto* type = asType(classInfo);
        if (isInstanceCheck && typeOf(globalObject, value) == type)
            return true;
        // Nearly every class leaves it to type.
        if (type->metatype() == realm->typeType()) {
            if (isInstanceCheck)
                return isInstance(globalObject, value, type);
            if (!isClass(value)) {
                raiseTypeError(globalObject, scope, "issubclass() arg 1 must be a class"_s);
                return false;
            }
            return asType(value)->isSubtypeOf(type);
        }
    }

    JSValue self;
    JSValue method = lookupSpecial(globalObject, classInfo, isInstanceCheck ? names.dunder_instancecheck : names.dunder_subclasscheck, self);
    RETURN_IF_EXCEPTION(scope, false);
    if (method) {
        JSValue result = callMethod(globalObject, method, self, value);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
    }
    raiseTypeError(globalObject, scope, isInstanceCheck ? "isinstance() arg 2 must be a type, a tuple of types, or a union"_s : "issubclass() arg 2 must be a class, a tuple of classes, or a union"_s);
    return false;
}

bool isInstanceOf(JSGlobalObject* globalObject, JSValue value, JSValue classInfo)
{
    return checkClassInfo(globalObject, value, classInfo, true);
}

bool isSubclassOf(JSGlobalObject* globalObject, JSValue value, JSValue classInfo)
{
    return checkClassInfo(globalObject, value, classInfo, false);
}

} // namespace Python

JSC_DEFINE_HOST_FUNCTION(pythonClassWasDefined, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSValue constructor = callFrame->argument(0);
    if (!constructor.isCell() || !Python::isJavaScriptClass(constructor.asCell()))
        return JSValue::encode(jsUndefined());
    // From now on it is known to Python, which is how a class that extends it will be seen to be derived from one of Python's.
    PyType* type = Python::classFor(asObject(constructor));
    if (!type->hasFlag(PyType::IsJavaScript))
        Python::javaScriptClassWasDefined(globalObject, type);
    return JSValue::encode(jsUndefined());
}

} // namespace JSC
