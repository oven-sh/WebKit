/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyContextVars.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include <errno.h>

namespace JSC {
namespace Python {

// The module _contextvars. (Python/context.c in CPython is the reference.)
//
// A context maps variables to values. The mapping is a dict that is never changed once a context has it: setting a variable
// gives the context a changed copy. So copying a context, which event loops do for every callback, shares the dict.

enum ContextField : unsigned { ContextValues, ContextPrevious, ContextFieldCount }; // number: whether it is entered
enum ContextVarField : unsigned { ContextVarName, ContextVarDefault, ContextVarFieldCount };
enum ContextTokenField : unsigned { TokenContext, TokenVar, TokenOldValue, TokenFieldCount }; // number: whether it is used

static PyType* contextType(JSGlobalObject*);
static PyType* contextVarType(JSGlobalObject*);
static PyType* contextTokenType(JSGlobalObject*);

static PyRecord* newContext(JSGlobalObject* globalObject, PyDict* values, PyType* type = nullptr)
{
    PyRecord* context = PyRecord::create(globalObject, type ? type : contextType(globalObject), ContextFieldCount);
    context->set(getVM(globalObject), ContextValues, values ? values : PyDict::create(globalObject));
    return context;
}

static PyRecord* currentContext(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (JSValue context = rt.currentContext())
        return PyRecord::from(context);
    PyRecord* context = newContext(globalObject, nullptr);
    rt.setCurrentContext(context);
    return context;
}

static ALWAYS_INLINE PyDict* contextValues(PyRecord* context) { return PyDict::from(context->get(ContextValues)); }

// ---- Context ----

static JSValue contextNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount())
        return raiseTypeError(globalObject, scope, "Context() does not accept any arguments"_s);
    RELEASE_AND_RETURN(scope, newContext(globalObject, nullptr));
}

bool enterContext(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* context = PyRecord::from(self);
    if (context->number) {
        raiseRuntimeError(globalObject, scope, "cannot enter context: "_s, reprForMessage(globalObject, self), " is already entered"_s);
        return false;
    }
    context->number = 1;
    context->set(vm, ContextPrevious, rt.currentContext());
    rt.setCurrentContext(context);
    return true;
}

bool exitContext(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* context = PyRecord::from(self);
    if (!context->number) {
        raiseRuntimeError(globalObject, scope, "cannot exit context: "_s, reprForMessage(globalObject, self), " has not been entered"_s);
        return false;
    }
    if (rt.currentContext() != self) {
        raiseRuntimeError(globalObject, scope, "cannot exit context: thread state references a different context object"_s);
        return false;
    }
    rt.setCurrentContext(context->get(ContextPrevious));
    context->set(vm, ContextPrevious, { });
    context->number = 0;
    return true;
}

OutermostContextScope::OutermostContextScope(JSGlobalObject* globalObject)
    : m_globalObject(globalObject)
{
    Runtime& rt = runtime(globalObject);
    JSValue current = rt.currentContext();
    if (!current || !PyRecord::from(current)->number) [[likely]]
        return;
    m_current = current;
    auto before = [&](PyRecord* context) {
        JSValue previous = context->get(ContextPrevious);
        // It is made when it is first asked for, and nobody had yet when the first was entered.
        if (!previous) {
            previous = newContext(globalObject, nullptr);
            context->set(rt.vm(), ContextPrevious, previous);
        }
        return PyRecord::from(previous);
    };
    // The one that is not entered. But a program that has got hold of that one (gc hands it out) may have entered it on top of the others: then all are entered, the way back is a circle, and it is where
    // the circle is come into. (Two walk, one at twice the pace; if they meet, it is as far from there as from the start.)
    PyRecord* start = PyRecord::from(current);
    PyRecord* slow = start;
    PyRecord* fast = start;
    do {
        for (unsigned i = 0; i < 2; ++i) {
            fast = before(fast);
            if (!fast->number) {
                rt.setCurrentContext(fast);
                return;
            }
        }
        slow = before(slow);
    } while (slow != fast);
    for (slow = start; slow != fast; fast = before(fast))
        slow = before(slow);
    rt.setCurrentContext(slow);
}

OutermostContextScope::~OutermostContextScope()
{
    if (!m_current) [[likely]]
        return;
    // (Whoever has waited goes by errno next.)
    int error = errno;
    runtime(m_globalObject).setCurrentContext(m_current);
    errno = error;
}

static JSValue contextRun(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!args.size())
        return raiseTypeError(globalObject, scope, "run() missing 1 required positional argument"_s);
    if (failed(scope, enterContext(globalObject, self)))
        return { };
    auto* context = PyRecord::from(self);
    scope.release();
    JSValue result = call(globalObject, args[0], args.withoutFirst());
    rt.setCurrentContext(context->get(ContextPrevious));
    context->set(vm, ContextPrevious, { });
    context->number = 0;
    return result;
}

static JSValue contextCopy(JSGlobalObject* globalObject, JSValue self) { return newContext(globalObject, contextValues(PyRecord::from(self))); }

static bool checkContextKey(JSGlobalObject* globalObject, JSValue key)
{
    if (PyRecord::tryFrom(key, contextVarType(globalObject)))
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "a ContextVar key was expected, got "_s, reprForMessage(globalObject, key));
    return false;
}

static JSValue contextGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkContextKey(globalObject, key)))
        return { };
    JSValue value = contextValues(PyRecord::from(self))->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return raiseKeyError(globalObject, scope, key);
    return value;
}

static JSValue contextGet(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkContextKey(globalObject, key)))
        return { };
    JSValue value = contextValues(PyRecord::from(self))->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    return value ? value : defaultValue ? defaultValue : jsUndefined();
}

static JSValue contextContains(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkContextKey(globalObject, key)))
        return { };
    bool found = contextValues(PyRecord::from(self))->contains(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

static JSValue contextLen(JSGlobalObject*, JSValue self) { return jsNumber(contextValues(PyRecord::from(self))->size()); }
static JSValue contextIter(JSGlobalObject* globalObject, JSValue self) { return getIter(globalObject, contextValues(PyRecord::from(self))); }
static JSValue contextKeys(JSGlobalObject* globalObject, JSValue self) { return getIter(globalObject, contextValues(PyRecord::from(self))); }
static JSValue contextValuesMethod(JSGlobalObject* globalObject, JSValue self) { return getIter(globalObject, PyDictView::create(globalObject, PyDictView::Kind::Values, contextValues(PyRecord::from(self)))); }
static JSValue contextItems(JSGlobalObject* globalObject, JSValue self) { return getIter(globalObject, PyDictView::create(globalObject, PyDictView::Kind::Items, contextValues(PyRecord::from(self)))); }

template<CompareOperator op>
static JSValue contextEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* otherContext = PyRecord::tryFrom(other, contextType(globalObject));
    if (!otherContext)
        return runtime(globalObject).notImplemented();
    return richCompare(globalObject, contextValues(PyRecord::from(self)), contextValues(otherContext), op);
}

static constexpr NativeDef contextMethods[] = {
    PY_NEW(contextNew, "($type, *args, **kwargs)"),
    PY_SLOT("__getitem__", contextGetItem, "($self, key, /)"),
    PY_SLOT("__contains__", contextContains, "($self, key, /)"),
    PY_SLOT("__len__", contextLen, "($self, /)"),
    PY_SLOT("__iter__", contextIter, "($self, /)"),
    PY_SLOT("__eq__", contextEquality<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", contextEquality<CompareOperator::NotEq>, "($self, value, /)"),
    PY_METHOD("run", contextRun, "($self, *args, **kwargs)"),
    PY_METHOD("copy", contextCopy, "($self, /)"),
    PY_METHOD("get", contextGet, "($self, key, default=None, /)"),
    PY_METHOD("keys", contextKeys, "($self, /)"),
    PY_METHOD("values", contextValuesMethod, "($self, /)"),
    PY_METHOD("items", contextItems, "($self, /)"),
};

static constexpr TypeDef contextTypeDef { "_contextvars.Context"_s, { TypeFlag::HasWeakref, TypeFlag::OwnsContainers }, contextMethods, { }, { } };

static PyType* contextType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(contextTypeDef); }

bool isContext(JSGlobalObject* globalObject, JSValue value) { return isPyObject(value) && uncheckedDowncast<PyObject>(value.asCell())->type() == contextType(globalObject); }
JSValue copyCurrentContext(JSGlobalObject* globalObject)
{
    PyRecord* current = currentContext(globalObject);
    return newContext(globalObject, contextValues(current), current->type());
}

// ---- Token ----

static JSValue tokenMissing(JSGlobalObject*);

static JSValue tokenNew(JSGlobalObject* globalObject, JSValue, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseRuntimeError(globalObject, scope, "Tokens can only be created by ContextVars"_s);
}

static JSValue tokenRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* token = PyRecord::from(self);
    return makeStr(globalObject, makeString("<Token "_s, token->number ? "used "_s : ""_s, "var="_s, reprForMessage(globalObject, token->get(TokenVar)), " at "_s, addressForRepr(globalObject, self), '>'));
}

static JSValue tokenGetVar(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(TokenVar); }
static JSValue tokenGetOldValue(JSGlobalObject* globalObject, JSValue self)
{
    JSValue value = PyRecord::from(self)->get(TokenOldValue);
    return value ? value : tokenMissing(globalObject);
}

static JSValue tokenEnter(JSGlobalObject*, JSValue self) { return self; }
static JSValue contextVarReset(JSGlobalObject*, JSValue self, JSValue token);
static JSValue tokenExit(JSGlobalObject* globalObject, JSValue self, JSValue, JSValue, JSValue)
{
    return contextVarReset(globalObject, PyRecord::from(self)->get(TokenVar), self);
}

static constexpr NativeDef tokenMethods[] = {
    PY_NEW(tokenNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", tokenRepr, "($self, /)"),
    PY_METHOD("__enter__", tokenEnter, "($self, /)"),
    PY_METHOD("__exit__", tokenExit, "($self, type, val, tb, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet tokenGetSets[] = {
    { "var"_s, tokenGetVar },
    { "old_value"_s, tokenGetOldValue },
};

static constexpr TypeDef contextTokenTypeDef { "_contextvars.Token"_s, { }, tokenMethods, tokenGetSets, { } };

static JSValue tokenMissingRepr(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "<Token.MISSING>"_s); }

static constexpr NativeDef tokenMissingMethods[] = {
    PY_SLOT("__repr__", tokenMissingRepr, "($self, /)"),
};

static constexpr TypeDef tokenMissingTypeDef { "Token.MISSING"_s, { TypeFlag::NothingOfItsOwn }, tokenMissingMethods, { }, { } };

static PyType* contextTokenType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyType* type = rt.typeFor(contextTokenTypeDef);
    JSString* name = internString(vm, "MISSING"_s);
    if (!type->dict()->get(name)) {
        type->dict()->set(vm, name, PyRecord::create(globalObject, rt.typeFor(tokenMissingTypeDef), 0));
        type->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        type->didChange(vm);
    }
    return type;
}

static JSValue tokenMissing(JSGlobalObject* globalObject)
{
    return contextTokenType(globalObject)->dict()->get(internString(getVM(globalObject), "MISSING"_s));
}

// ---- ContextVar ----

// ContextVar(name, /, *, default=...)
static JSValue contextVarNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount() > 2)
        return raiseTypeError(globalObject, scope, "ContextVar() takes at most 2 "_s, args.size() ? ""_s : "keyword "_s, "arguments ("_s, args.totalCount(), " given)"_s);
    if (args.size() != 1)
        return raiseTypeError(globalObject, scope, "ContextVar() takes "_s, args.size() ? "at most"_s : "exactly"_s, " 1 positional argument ("_s, args.size(), " given)"_s);
    JSValue name = args[0];
    JSValue defaultValue;
    if (args.keywordCount()) {
        String keyword = stringOf(args.keywordName(0));
        if (keyword != "default"_s)
            RELEASE_AND_RETURN(scope, raiseUnexpectedKeyword(globalObject, "ContextVar"_s, keyword, { "default"_s }));
        defaultValue = args.keywordValue(0);
    }
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "context variable name must be a str"_s);
    // The hash of the variable is made of that of the name, in CPython. An instance of a class derived from str may have none.
    hash(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* variable = PyRecord::create(globalObject, contextVarType(globalObject), ContextVarFieldCount);
    variable->set(vm, ContextVarName, name);
    variable->set(vm, ContextVarDefault, defaultValue);
    return variable;
}

static JSValue contextVarGet(JSGlobalObject* globalObject, JSValue self, JSValue defaultValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue context = rt.currentContext()) {
        JSValue value = contextValues(PyRecord::from(context))->get(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }
    if (defaultValue)
        return defaultValue;
    if (JSValue value = PyRecord::from(self)->get(ContextVarDefault))
        return value;
    return raiseWithArgs(globalObject, scope, rt.LookupError(), { self });
}

static void replaceContextValue(JSGlobalObject* globalObject, PyRecord* context, JSValue variable, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* values = contextValues(context)->copy(globalObject);
    if (value)
        values->set(globalObject, variable, value);
    else
        values->remove(globalObject, variable);
    RETURN_IF_EXCEPTION(scope, void());
    context->set(vm, ContextValues, values);
    runtime(globalObject).didChangeContextVariables();
}

static JSValue contextVarSet(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* context = currentContext(globalObject);
    JSValue old = contextValues(context)->get(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* token = PyRecord::create(globalObject, contextTokenType(globalObject), TokenFieldCount);
    token->set(vm, TokenContext, context);
    token->set(vm, TokenVar, self);
    token->set(vm, TokenOldValue, old);
    replaceContextValue(globalObject, context, self, value);
    RETURN_IF_EXCEPTION(scope, { });
    return token;
}

JSValue newContextVariable(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = getVM(globalObject);
    PyRecord* variable = PyRecord::create(globalObject, contextVarType(globalObject), ContextVarFieldCount);
    variable->set(vm, ContextVarName, makeStr(vm, name));
    return variable;
}

JSValue contextVariableValue(JSGlobalObject* globalObject, JSValue variable)
{
    JSValue context = runtime(globalObject).currentContext();
    if (!context)
        return { };
    return contextValues(PyRecord::from(context))->get(globalObject, variable);
}

bool setContextVariable(JSGlobalObject* globalObject, JSValue variable, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    replaceContextValue(globalObject, currentContext(globalObject), variable, value);
    return !scope.exception();
}

static JSValue contextVarReset(JSGlobalObject* globalObject, JSValue self, JSValue tokenValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* token = PyRecord::tryFrom(tokenValue, contextTokenType(globalObject));
    if (!token)
        return raiseTypeError(globalObject, scope, "expected an instance of Token, got "_s, reprForMessage(globalObject, tokenValue));
    if (token->number)
        return raiseRuntimeError(globalObject, scope, reprForMessage(globalObject, tokenValue), " has already been used once"_s);
    if (token->get(TokenVar) != self)
        return raiseValueError(globalObject, scope, reprForMessage(globalObject, tokenValue), " was created by a different ContextVar"_s);
    PyRecord* context = currentContext(globalObject);
    if (token->get(TokenContext) != JSValue(context))
        return raiseValueError(globalObject, scope, reprForMessage(globalObject, tokenValue), " was created in a different Context"_s);
    token->number = 1;
    replaceContextValue(globalObject, context, self, token->get(TokenOldValue));
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue contextVarRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* variable = PyRecord::from(self);
    StrBuilder text;
    text.append("<ContextVar name="_s, reprForMessage(globalObject, variable->get(ContextVarName)));
    if (JSValue defaultValue = variable->get(ContextVarDefault))
        text.append(" default="_s, reprForMessage(globalObject, defaultValue));
    text.append(" at "_s, addressForRepr(globalObject, self), '>');
    return makeStr(globalObject, text);
}

static JSValue contextVarGetName(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(ContextVarName); }

static constexpr NativeDef contextVarMethods[] = {
    PY_NEW(contextVarNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", contextVarRepr, "($self, /)"),
    PY_METHOD("get", contextVarGet, "($self, default=<unrepresentable>, /)"),
    PY_METHOD("set", contextVarSet, "($self, value, /)"),
    PY_METHOD("reset", contextVarReset, "($self, token, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet contextVarGetSets[] = {
    PY_MEMBER("name", contextVarGetName),
};

static constexpr TypeDef contextVarTypeDef { "_contextvars.ContextVar"_s, { }, contextVarMethods, contextVarGetSets, { } };

static PyType* contextVarType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(contextVarTypeDef); }

// ---- The module ----

static JSValue contextvarsCopyContext(JSGlobalObject* globalObject) { return copyCurrentContext(globalObject); }

static constexpr NativeDef contextvarsFunctions[] = {
    PY_FUNCTION("copy_context", contextvarsCopyContext, "()"),
};

void initializeContextVarsModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    module->addFunctions(globalObject, contextvarsFunctions);
    PyType* context = contextType(globalObject);
    context->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
    context->didChange(vm);
    module->add(globalObject, "Context"_s, context);
    module->add(globalObject, "ContextVar"_s, contextVarType(globalObject));
    module->add(globalObject, "Token"_s, contextTokenType(globalObject));
}

} // namespace Python
} // namespace JSC
