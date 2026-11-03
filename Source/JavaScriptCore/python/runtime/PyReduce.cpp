/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyList.h"
#include "PyStr.h"

namespace JSC {
namespace Python {

// object.__reduce__, object.__reduce_ex__ and object.__getstate__: how pickle and copy take apart an object whose class
// says nothing of it. (Objects/typeobject.c in CPython is the reference.)
//
// The module copyreg does what is left of the work: protocols 0 and 1, finding the names of the slots of a class, and the
// functions __newobj__ and __newobj_ex__, which stand for "call cls.__new__".

static JSValue reduceImportCopyreg(JSGlobalObject* globalObject)
{
    return importModule(globalObject, "copyreg"_s);
}

// The names of all the slots of the instances of the class: a list, or None.
static JSValue reduceSlotNames(JSGlobalObject* globalObject, PyType* cls)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    // copyreg leaves them in the class the first time.
    if (JSValue cached = cls->dict()->get(internString(vm, "__slotnames__"_s))) {
        if (!isNone(cached) && !isList(cached))
            return raiseTypeError(globalObject, scope, cls->name(), ".__slotnames__ should be a list or None, not "_s, typeName(globalObject, cached));
        return cached;
    }

    JSValue copyreg = reduceImportCopyreg(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue names = callMethod(globalObject, copyreg, internString(vm, "_slotnames"_s), cls);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(names) && !isList(names))
        return raiseTypeError(globalObject, scope, "copyreg._slotnames didn't return a list or None"_s);
    return names;
}

// Whether the instances are of no fixed size, as CPython lays them out.
static bool reduceHasItems(JSGlobalObject* globalObject, PyType* type)
{
    return type->flags().containsAny({ TypeFlag::IntSubclass, TypeFlag::TupleSubclass, TypeFlag::BytesSubclass, TypeFlag::TypeSubclass }) || type == runtime(globalObject).codeType() || type == runtime(globalObject).memoryviewType();
}

// Whether the instances hold something besides a __dict__ and slots, which __new__ without arguments could not bring back.
static bool reduceHasNativeState(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    while (type->isHeapType())
        type = type->base();
    // A type written in C++ that leaves it to object.__new__() to make its instances, as _io._IOBase, adds nothing to them.
    if (auto* native = PyNativeFunction::tryFrom(type->lookup(Special::New)); native && !type->has(TypeFlag::DisallowInstantiation)) {
        auto& def = native->def();
        if (def.signature.isVariadic && def.signature.hasSelf && def.pointer.variadicWithSelf == objectNew)
            return false;
    }
    return type != rt.objectType() && type != rt.noneType() && type != rt.notImplementedType() && type != rt.ellipsisType();
}

// `required`: the state is all there will be to make the copy from, so what it cannot hold is an error.
static JSValue reduceDefaultState(JSGlobalObject* globalObject, JSValue object, bool required)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(object);

    if (required && reduceHasItems(globalObject, type))
        return raiseTypeError(globalObject, scope, "cannot pickle "_s, type->nameInMessages(), " objects"_s);

    JSValue state = jsUndefined();
    if (PyDict* dict = instanceDict(globalObject, object, false); dict && !dict->isEmpty())
        state = dict;

    JSValue namesValue = reduceSlotNames(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (required && reduceHasNativeState(globalObject, type))
        return raiseTypeError(globalObject, scope, "cannot pickle '"_s, type->nameInMessages(), "' object"_s);

    JSArray* names = tryAsList(namesValue);
    if (!names || !listLength(names))
        return state;

    PyDict* slots = PyDict::create(globalObject);
    unsigned count = listLength(names);
    for (unsigned i = 0; i < count; ++i) {
        JSString* name = attributeName(globalObject, listGet(names, i));
        RETURN_IF_EXCEPTION(scope, { });
        // A slot without a value is no error.
        JSValue value = getAttrIfPresent(globalObject, object, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            slots->set(vm, name, value);
        // The list belongs to the class, and anything may have run.
        if (listLength(names) != count)
            return raiseRuntimeError(globalObject, scope, "__slotnames__ changed size during iteration"_s);
    }
    if (slots->isEmpty())
        return state;
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { state, JSValue(slots) }));
}

JSValue objectGetState(JSGlobalObject* globalObject, JSValue self)
{
    return reduceDefaultState(globalObject, self, false);
}

// object.__getstate__()
static JSValue reduceGetState(JSGlobalObject* globalObject, JSValue object, bool required)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = getAttr(globalObject, object, runtime(globalObject).names.dGetState());
    RETURN_IF_EXCEPTION(scope, { });
    // The one above can be told what is required of it.
    if (auto* native = PyNativeFunction::tryFrom(method)) {
        auto& signature = native->def().signature;
        if (native->self() && isIdentical(native->self(), object) && signature.hasSelf && !signature.isVariadic && !signature.parameterCount && native->def().pointer.f1 == objectGetState)
            RELEASE_AND_RETURN(scope, reduceDefaultState(globalObject, object, required));
    }
    RELEASE_AND_RETURN(scope, call(globalObject, method));
}

// object.__getstate__(), whatever the class has made of it: the state the __reduce__ of a built-in type reports.
JSValue getObjectState(JSGlobalObject* globalObject, JSValue object)
{
    return reduceGetState(globalObject, object, false);
}

// type(object).name, bound to the object. Empty, with no exception: there is none.
static JSValue reduceLookupSpecial(JSGlobalObject* globalObject, JSValue object, JSString* name)
{
    PyType* type = typeOf(globalObject, object);
    JSValue descriptor = type->lookup(name);
    if (!descriptor)
        return { };
    return bindDescriptor(globalObject, descriptor, object, type);
}

// The arguments to give __new__, from __getnewargs_ex__ or __getnewargs__. Null where the object has none to give.
// False: an exception.
static bool reduceNewArguments(JSGlobalObject* globalObject, JSValue object, PyTuple*& args, PyDict*& kwargs)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    args = nullptr;
    kwargs = nullptr;

    JSValue method = reduceLookupSpecial(globalObject, object, rt.names.dGetNewArgsEx());
    RETURN_IF_EXCEPTION(scope, false);
    if (method) {
        JSValue result = call(globalObject, method);
        RETURN_IF_EXCEPTION(scope, false);
        auto* pair = PyTuple::tryFrom(result);
        if (!pair) {
            raiseTypeError(globalObject, scope, "__getnewargs_ex__ should return a tuple, not '"_s, typeName(globalObject, result), '\'');
            return false;
        }
        if (pair->length() != 2) {
            raiseValueError(globalObject, scope, "__getnewargs_ex__ should return a tuple of length 2, not "_s, pair->length());
            return false;
        }
        if (!PyTuple::is(pair->at(0))) {
            raiseTypeError(globalObject, scope, "first item of the tuple returned by __getnewargs_ex__ must be a tuple, not '"_s, typeName(globalObject, pair->at(0)), '\'');
            return false;
        }
        if (!isDict(pair->at(1))) {
            raiseTypeError(globalObject, scope, "second item of the tuple returned by __getnewargs_ex__ must be a dict, not '"_s, typeName(globalObject, pair->at(1)), '\'');
            return false;
        }
        args = PyTuple::from(pair->at(0));
        kwargs = PyDict::from(pair->at(1));
        return true;
    }

    method = reduceLookupSpecial(globalObject, object, rt.names.dGetNewArgs());
    RETURN_IF_EXCEPTION(scope, false);
    if (method) {
        JSValue result = call(globalObject, method);
        RETURN_IF_EXCEPTION(scope, false);
        args = PyTuple::tryFrom(result);
        if (!args) {
            raiseTypeError(globalObject, scope, "__getnewargs__ should return a tuple, not '"_s, typeName(globalObject, result), '\'');
            return false;
        }
    }
    return true;
}

// For protocol 2 and up: (copyreg.__newobj__, (cls, *args), state, list items, dict items)
static JSValue reduceNewObject(JSGlobalObject* globalObject, JSValue object)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(object);
    if (type->has(TypeFlag::DisallowInstantiation))
        return raiseTypeError(globalObject, scope, "cannot pickle '"_s, type->nameInMessages(), "' object"_s);

    PyTuple* args;
    PyDict* kwargs;
    if (failed(scope, reduceNewArguments(globalObject, object, args, kwargs)))
        return { };
    JSValue copyreg = reduceImportCopyreg(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue function;
    PyTuple* functionArguments;
    if (!kwargs || kwargs->isEmpty()) {
        function = getAttr(globalObject, copyreg, internString(vm, "__newobj__"_s));
        RETURN_IF_EXCEPTION(scope, { });
        unsigned count = args ? args->length() : 0;
        functionArguments = PyTuple::create(globalObject, count + 1);
        functionArguments->initializeAt(vm, 0, type);
        for (unsigned i = 0; i < count; ++i)
            functionArguments->initializeAt(vm, i + 1, args->at(i));
    } else {
        function = getAttr(globalObject, copyreg, internString(vm, "__newobj_ex__"_s));
        RETURN_IF_EXCEPTION(scope, { });
        functionArguments = PyTuple::create(globalObject, { JSValue(type), JSValue(args), JSValue(kwargs) });
    }

    bool isListObject = isList(object);
    bool isDictObject = isDict(object);
    JSValue state = reduceGetState(globalObject, object, !(args || isListObject || isDictObject));
    RETURN_IF_EXCEPTION(scope, { });

    JSValue listItems = jsUndefined();
    if (isListObject) {
        listItems = getIter(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue dictItems = jsUndefined();
    if (isDictObject) {
        JSValue items = callMethod(globalObject, object, rt.names.items());
        RETURN_IF_EXCEPTION(scope, { });
        dictItems = getIter(globalObject, items);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { function, JSValue(functionArguments), state, listItems, dictItems }));
}

static JSValue reduceCommon(JSGlobalObject* globalObject, JSValue object, int protocol)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (protocol >= 2)
        RELEASE_AND_RETURN(scope, reduceNewObject(globalObject, object));
    JSValue copyreg = reduceImportCopyreg(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethod(globalObject, copyreg, internString(vm, "_reduce_ex"_s), object, jsNumber(protocol)));
}

JSValue objectReduce(JSGlobalObject* globalObject, JSValue self)
{
    return reduceCommon(globalObject, self, 0);
}

JSValue objectReduceEx(JSGlobalObject* globalObject, JSValue self, JSValue protocolValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int protocol = toCInt(globalObject, protocolValue);
    RETURN_IF_EXCEPTION(scope, { });

    // A class that has its own __reduce__ means it to be used whatever the protocol.
    JSValue reduce = getAttrIfPresent(globalObject, self, rt.names.dReduce());
    RETURN_IF_EXCEPTION(scope, { });
    if (reduce) {
        JSValue ofClass = getAttr(globalObject, rt.typeOf(self), rt.names.dReduce());
        RETURN_IF_EXCEPTION(scope, { });
        if (ofClass != rt.objectType()->dict()->get(rt.names.dReduce()))
            RELEASE_AND_RETURN(scope, call(globalObject, reduce));
    }
    RELEASE_AND_RETURN(scope, reduceCommon(globalObject, self, protocol));
}

} // namespace Python
} // namespace JSC
