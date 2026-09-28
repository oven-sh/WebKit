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
#include "PythonOperations.h"

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

    Vector<Vector<PyType*, 8>, 4> sequences;
    for (auto& base : bases->span()) {
        Vector<PyType*, 8> order;
        for (auto& entry : uncheckedDowncast<PyType>(base.get().asCell())->mro()->span())
            order.append(uncheckedDowncast<PyType>(entry.get().asCell()));
        sequences.append(WTF::move(order));
    }
    Vector<PyType*, 8> given;
    for (auto& base : bases->span())
        given.append(uncheckedDowncast<PyType>(base.get().asCell()));
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
            for (size_t i = 0; i < sequences.size(); ++i) {
                if (positions[i] >= sequences[i].size())
                    continue;
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

// The base whose instances are laid out as the new class's will be. All the others have to be content with that.
static PyType* bestBase(JSGlobalObject* globalObject, PyTuple* bases)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* winner = nullptr;
    for (auto& entry : bases->span()) {
        if (!isType(entry.get())) {
            raiseTypeError(globalObject, scope, "bases must be types"_s);
            return nullptr;
        }
        auto* base = uncheckedDowncast<PyType>(entry.get().asCell());
        if (!base->hasFlag(PyType::IsBaseType)) {
            raiseTypeError(globalObject, scope, makeString("type '"_s, base->nameString(globalObject), "' is not an acceptable base type"_s));
            return nullptr;
        }
        if (!winner || (winner->layout() == PyType::Layout::Object && base->layout() != PyType::Layout::Object)) {
            winner = base;
            continue;
        }
        if (base->layout() != PyType::Layout::Object && base->layout() != winner->layout()) {
            raiseTypeError(globalObject, scope, "multiple bases have instance lay-out conflict"_s);
            return nullptr;
        }
    }
    return winner;
}

// ---- type.__new__

static bool callSetNames(JSGlobalObject* globalObject, PyType* type, PyDict* namespaceDict)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (unsigned entry = 0; entry < namespaceDict->entryCount(); ++entry) {
        JSValue key = namespaceDict->keyAt(entry);
        if (!key)
            continue;
        JSValue value = namespaceDict->valueAt(entry);
        if (!value.isObject())
            continue;
        JSValue self;
        JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_set_name, self);
        RETURN_IF_EXCEPTION(scope, false);
        if (!method)
            continue;
        callMethod(globalObject, method, self, type, key);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

static JSValue callWithKeywordDict(JSGlobalObject* globalObject, JSValue callable, MarkedArgumentBuffer& arguments, PyDict* keywords)
{
    VM& vm = globalObject->vm();
    if (!keywords || !keywords->size())
        return call(globalObject, callable, arguments);
    KeywordNames* names = KeywordNames::create(vm, CopyOnWriteArrayWithContiguous, keywords->size());
    unsigned i = 0;
    for (unsigned entry = 0; entry < keywords->entryCount(); ++entry) {
        JSValue key = keywords->keyAt(entry);
        if (!key)
            continue;
        names->setIndex(vm, i++, key);
        arguments.append(keywords->valueAt(entry));
    }
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

    PyType* type = PyType::create(vm, globalObject, metatype, name, bases, base, order);

    bool hasSlots = false;
    bool slotsIncludeDict = false;
    for (unsigned entry = 0; entry < namespaceDict->entryCount(); ++entry) {
        JSValue key = namespaceDict->keyAt(entry);
        if (!key)
            continue;
        if (!key.isString())
            continue; // FIXME: It should still be in __dict__.
        JSValue value = namespaceDict->valueAt(entry);
        auto property = asString(key)->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, { });

        // These are what they are whether or not they are decorated as such.
        bool isPlainFunction = value.isCell() && value.asCell()->type() == JSFunctionType && !value.asCell()->inherits<PyNativeFunction>();
        if (isPlainFunction && property == names.dunder_new)
            value = PyNativeObject::create(globalObject, BuiltinType::StaticMethod, value);
        else if (isPlainFunction && (property == names.dunder_init_subclass || property == names.dunder_class_getitem))
            value = PyNativeObject::create(globalObject, BuiltinType::ClassMethod, value);

        if (property == names.dunder_slots) {
            hasSlots = true;
            MarkedArgumentBuffer slotNames;
            if (value.isString())
                slotNames.append(value);
            else {
                collect(globalObject, value, slotNames);
                RETURN_IF_EXCEPTION(scope, { });
            }
            for (unsigned i = 0; i < slotNames.size(); ++i) {
                if (!slotNames.at(i).isString())
                    return raiseTypeError(globalObject, scope, makeString("__slots__ items must be strings, not '"_s, typeName(globalObject, slotNames.at(i)), '\''));
                auto slot = asString(slotNames.at(i))->toIdentifier(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
                if (slot == names.dunder_dict) {
                    slotsIncludeDict = true;
                    continue;
                }
                if (slot == names.dunder_weakref)
                    continue;
                type->putDirect(vm, slot, PyNativeObject::create(globalObject, BuiltinType::MemberDescriptor, slotNames.at(i), type));
            }
        }
        type->putDirect(vm, property, value);
    }

    // No attributes but the slots, if none of the bases' instances have any either.
    if (hasSlots && !slotsIncludeDict) {
        bool basesHaveDict = false;
        for (auto& entry : order->span()) {
            auto* ancestor = uncheckedDowncast<PyType>(entry.get().asCell());
            basesHaveDict |= ancestor->hasFlag(PyType::IsHeapType) && !ancestor->hasFlag(PyType::HasNoInstanceDict);
        }
        if (!basesHaveDict)
            type->setFlag(PyType::HasNoInstanceDict);
    }

    // What says when two of them are equal, and not what their hash is, cannot be hashed.
    if (type->lookupOwn(vm, names.dunder_eq) && !type->lookupOwn(vm, names.dunder_hash))
        type->putDirect(vm, names.dunder_hash, jsUndefined());
    if (!type->lookupOwn(vm, names.dunder_doc))
        type->putDirect(vm, names.dunder_doc, jsUndefined());
    if (!type->lookupOwn(vm, names.dunder_qualname))
        type->putDirect(vm, names.dunder_qualname, name);
    ++names.typeEpoch;

    callSetNames(globalObject, type, namespaceDict);
    RETURN_IF_EXCEPTION(scope, { });

    // super().__init_subclass__(**keywords)
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
        if (isType(base)) {
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
    if (isType(metaclass)) {
        metaclass = calculateMetaclass(globalObject, uncheckedDowncast<PyType>(metaclass.asCell()), bases);
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
    }
    return result;
}

// ---- super

JSValue getSuperAttribute(JSGlobalObject* globalObject, JSValue superObject, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto* object = uncheckedDowncast<PyNativeObject>(superObject.asCell());
    JSValue after = object->field(0);
    JSValue instance = object->field(1);
    JSValue startType = object->field(2);
    // super().__class__ is super.
    if (!startType || !isType(startType) || name == vm.pythonNames().dunder_class)
        return { };
    auto* start = uncheckedDowncast<PyType>(startType.asCell());
    JSValue attribute = start->lookupAfter(vm, uncheckedDowncast<PyType>(after.asCell()), name);
    if (!attribute)
        return { };
    // super(C, D) where D is a class: what is found is got from the class, and not from an instance.
    bool isForClass = instance.isCell() && instance.asCell() == start;
    return bindDescriptor(globalObject, attribute, isForClass ? JSValue() : instance, start);
}

// ---- isinstance() and issubclass()

static bool checkClassInfo(JSGlobalObject* globalObject, JSValue value, JSValue classInfo, bool isInstanceCheck)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();

    if (isTuple(classInfo)) {
        for (auto& entry : uncheckedDowncast<PyTuple>(classInfo.asCell())->span()) {
            bool result = checkClassInfo(globalObject, value, entry.get(), isInstanceCheck);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return true;
        }
        return false;
    }

    if (isType(classInfo)) {
        auto* type = uncheckedDowncast<PyType>(classInfo.asCell());
        if (isInstanceCheck && typeOf(globalObject, value) == type)
            return true;
        // Nearly every class leaves it to type.
        if (type->metatype() == realm->typeType()) {
            if (isInstanceCheck)
                return isInstance(globalObject, value, type);
            if (!isType(value)) {
                raiseTypeError(globalObject, scope, "issubclass() arg 1 must be a class"_s);
                return false;
            }
            return uncheckedDowncast<PyType>(value.asCell())->isSubtypeOf(type);
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

} } // namespace JSC::Python
