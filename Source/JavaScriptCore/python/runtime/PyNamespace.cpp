/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyNamespace.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyStr.h"
#include "PyTypes.h"

namespace JSC {
namespace Python {

// sentinel and types.SimpleNamespace. (Objects/sentinelobject.c and Objects/namespaceobject.c in CPython are the reference.)

// ---- sentinel ----
//
// A record.

namespace {

enum SentinelField : unsigned {
    SentinelName,
    SentinelModule, // Empty once deleted.
    SentinelRepr, // Empty: the name.
    SentinelFieldCount,
};

}

static ALWAYS_INLINE PyRecord* sentinelRecord(JSValue value)
{
    ASSERT(PyRecord::is(value));
    return uncheckedDowncast<PyRecord>(value.asCell());
}

static PyRecord* sentinelCreate(JSGlobalObject* globalObject, PyType* type, JSValue name, JSValue module, JSValue repr)
{
    VM& vm = getVM(globalObject);
    PyRecord* sentinel = PyRecord::create(globalObject, type, SentinelFieldCount);
    sentinel->set(vm, SentinelName, name);
    sentinel->set(vm, SentinelModule, module);
    sentinel->set(vm, SentinelRepr, repr);
    return sentinel;
}

JSValue makeSentinel(JSGlobalObject* globalObject, const String& name, const String& module, const String& repr)
{
    VM& vm = getVM(globalObject);
    return sentinelCreate(globalObject, runtime(globalObject).sentinelType(), makeStr(vm, name), module.isNull() ? jsUndefined() : JSValue(makeStr(vm, module)), repr.isNull() ? JSValue() : JSValue(makeStr(vm, repr)));
}

// sentinel(name, /, *, repr=None)
static JSValue sentinelNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned total = args.totalCount();
    if (total > 2)
        return raiseTypeError(globalObject, scope, "sentinel() takes at most 2 "_s, args.isEmpty() ? "keyword "_s : ""_s, "arguments ("_s, total, " given)"_s);
    if (args.size() != 1)
        return raiseTypeError(globalObject, scope, "sentinel() takes exactly 1 positional argument ("_s, args.size(), " given)"_s);
    JSValue repr;
    if (args.keywordCount()) {
        String keyword = args.keywordName(0)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (keyword != "repr"_s)
            RELEASE_AND_RETURN(scope, raiseUnexpectedKeyword(globalObject, "sentinel"_s, keyword, { "repr"_s }));
        repr = args.keywordValue(0);
    }
    JSValue name = args[0];
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "sentinel() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, name));
    if (repr && isNone(repr))
        repr = JSValue();
    if (repr && !isStr(repr))
        return raiseTypeError(globalObject, scope, "sentinel() argument 'repr' must be str or None, not "_s, typeName(globalObject, repr));

    // It belongs to the module of the function that makes it, which is where pickle will look for it.
    JSValue module;
    if (PyFrame* frame = currentFrame(globalObject))
        module = frame->function()->codeOwner()->module();
    RELEASE_AND_RETURN(scope, sentinelCreate(globalObject, PyType::from(cls), name, module ? module : jsUndefined(), repr));
}

static JSValue sentinelRepr(JSGlobalObject*, JSValue self)
{
    PyRecord* sentinel = sentinelRecord(self);
    JSValue repr = sentinel->get(SentinelRepr);
    return repr ? repr : sentinel->get(SentinelName);
}

static JSValue sentinelOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, self, other); }
static JSValue sentinelROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, other, self); }
static JSValue sentinelCopy(JSGlobalObject*, JSValue self) { return self; }
static JSValue sentinelDeepCopy(JSGlobalObject*, JSValue self, JSValue) { return self; }
static JSValue sentinelReduce(JSGlobalObject*, JSValue self) { return sentinelRecord(self)->get(SentinelName); }
static JSValue sentinelGetName(JSGlobalObject*, JSValue self) { return sentinelRecord(self)->get(SentinelName); }

static JSValue sentinelGetModule(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue module = sentinelRecord(self)->get(SentinelModule))
        return module;
    return raiseAttributeError(globalObject, scope, self, runtime(globalObject).names.dModule(), "'sentinel' object has no attribute '__module__'"_s);
}

static void sentinelSetModule(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* sentinel = sentinelRecord(self);
    if (!value && !sentinel->get(SentinelModule)) {
        raiseAttributeError(globalObject, scope, self, runtime(globalObject).names.dModule(), "'sentinel' object has no attribute '__module__'"_s);
        return;
    }
    sentinel->set(vm, SentinelModule, value);
}

static constexpr NativeDef sentinelMethods[] = {
    PY_NEW(sentinelNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", sentinelRepr, "($self, /)"),
    PY_SLOT("__or__", sentinelOr, "($self, value, /)"),
    PY_SLOT("__ror__", sentinelROr, "($self, value, /)"),
    PY_METHOD("__copy__", sentinelCopy, "($self, /)"),
    PY_METHOD("__deepcopy__", sentinelDeepCopy, "($self, object, /)"),
    PY_METHOD("__reduce__", sentinelReduce, "($self, /)"),
};

static constexpr NativeGetSet sentinelGetSets[] = {
    PY_MEMBER("__name__", sentinelGetName),
    PY_WRITABLE_MEMBER("__module__", sentinelGetModule, sentinelSetModule),
};

// ---- types.SimpleNamespace ----
//
// An object with a __dict__ and nothing else, which is what a PyInstance is.

static bool isSimpleNamespace(JSGlobalObject* globalObject, JSValue value)
{
    return PyInstance::is(value) && PyInstance::from(value)->type()->isSubtypeOf(runtime(globalObject).simpleNamespaceType());
}

static ALWAYS_INLINE PyDict* namespaceDict(JSGlobalObject* globalObject, JSValue self)
{
    ASSERT(isSimpleNamespace(globalObject, self));
    return instanceDict(globalObject, self, true);
}

static PyInstance* namespaceCreate(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    return PyInstance::create(rt.vm(), type);
}

JSValue makeSimpleNamespace(JSGlobalObject* globalObject, PyDict* attributes)
{
    PyInstance* result = namespaceCreate(globalObject, runtime(globalObject).simpleNamespaceType());
    if (attributes)
        result->setDict(globalObject, attributes->copy(globalObject));
    return result;
}

static JSValue namespaceNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    return namespaceCreate(globalObject, PyType::from(cls));
}

// The keyword arguments of the call become attributes.
static bool namespaceAddKeywords(JSGlobalObject* globalObject, PyDict* dict, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        dict->set(globalObject, args.keywordName(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// SimpleNamespace(mapping_or_iterable=(), /, **kwargs)
static JSValue namespaceInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() > 1)
        return raiseTypeError(globalObject, scope, typeOf(globalObject, self)->name(), " expected at most 1 argument, got "_s, args.size());
    PyDict* dict = namespaceDict(globalObject, self);
    if (args.size()) {
        JSValue source = args[0];
        if (!PyDict::is(source) || PyDict::from(source)->type() != rt.dictType()) {
            source = call(globalObject, rt.dictType(), source);
            RETURN_IF_EXCEPTION(scope, { });
        }
        JSValue key;
        JSValue value;
        for (unsigned position = 0; PyDict::from(source)->next(position, key, value);) {
            if (!isStr(key))
                return raiseTypeError(globalObject, scope, "keywords must be strings"_s);
        }
        dictUpdate(globalObject, dict, source);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (failed(scope, namespaceAddKeywords(globalObject, dict, args)))
        return { };
    return jsUndefined();
}

static JSValue namespaceRepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyInstance::from(self)->type();
    String name = type == rt.simpleNamespaceType() ? "namespace"_str : type->name();
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, makeString(name, "(...)"_s));

    // The repr() of a value may change the namespace.
    PyDict* dict = namespaceDict(globalObject, self);
    JSArray* keys = listFromIterable(globalObject, dict);
    RETURN_IF_EXCEPTION(scope, { });
    StrBuilder out;
    out.append(name, '(');
    bool isFirst = true;
    for (unsigned i = 0; i < listLength(keys); ++i) {
        JSValue key = listGet(keys, i);
        if (!isStr(key))
            continue;
        String keyText = strValue(key)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (keyText.isEmpty())
            continue;
        JSValue value = dict->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            continue;
        JSString* valueRepr = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        String valueText = valueRepr->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isFirst)
            out.append(", "_s);
        isFirst = false;
        out.append(keyText, '=', valueText);
    }
    out.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, out));
}

template<CompareOperator op>
static JSValue namespaceCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!isSimpleNamespace(globalObject, other))
        return runtime(globalObject).notImplemented();
    return richCompare(globalObject, namespaceDict(globalObject, self), namespaceDict(globalObject, other), op);
}

static JSValue namespaceReduce(JSGlobalObject* globalObject, JSValue self)
{
    return PyTuple::create(globalObject, { JSValue(PyInstance::from(self)->type()), JSValue(runtime(globalObject).emptyTuple()), JSValue(namespaceDict(globalObject, self)) });
}

// "module.qualname", without the module if that is builtins or __main__.
static String namespaceFullTypeName(JSGlobalObject* globalObject, PyType* type)
{
    String name = qualifiedTypeName(globalObject, type);
    return name.startsWith("__main__."_s) ? name.substring(9) : name;
}

static JSValue namespaceReplace(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size())
        return raiseTypeError(globalObject, scope, "__replace__() takes no positional arguments"_s);
    PyType* type = PyInstance::from(self)->type();
    JSValue result = call(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isSimpleNamespace(globalObject, result))
        return raiseTypeError(globalObject, scope, "expect "_s, namespaceFullTypeName(globalObject, rt.simpleNamespaceType()), " type, but "_s, namespaceFullTypeName(globalObject, type), "() returned '"_s, namespaceFullTypeName(globalObject, rt.typeOf(result)), "' object"_s);
    PyDict* dict = namespaceDict(globalObject, result);
    dictUpdate(globalObject, dict, namespaceDict(globalObject, self));
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, namespaceAddKeywords(globalObject, dict, args)))
        return { };
    return result;
}

static JSValue namespaceGetDict(JSGlobalObject* globalObject, JSValue self) { return namespaceDict(globalObject, self); }

static constexpr NativeDef namespaceMethods[] = {
    PY_NEW(namespaceNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", namespaceInit, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", namespaceRepr, "($self, /)"),
    PY_SLOT("__eq__", namespaceCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", namespaceCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_METHOD("__reduce__", namespaceReduce, "($self, /)"),
    PY_METHOD("__replace__", namespaceReplace, "($self, /, **changes)"),
};

static constexpr NativeGetSet namespaceGetSets[] = {
    PY_MEMBER("__dict__", namespaceGetDict),
};

// ---- The types ----

void initializeNamespaceTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    rt.sentinelType()->initializeBuiltin(globalObject, { "sentinel"_s, { }, sentinelMethods, sentinelGetSets, "Create a unique sentinel object with the given name."_s }, rt.typeType(), rt.objectType());

    PyType* type = rt.simpleNamespaceType();
    type->initializeBuiltin(globalObject, { "types.SimpleNamespace"_s, { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::OwnLayout, TypeFlag::NothingOfItsOwn }, namespaceMethods, namespaceGetSets, "A simple attribute-based namespace."_s }, rt.typeType(), rt.objectType());
    // What compares by value and can change is not hashable.
    type->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
    type->didChange(vm);
}

} // namespace Python
} // namespace JSC
