/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyException.h"

#include "ErrorInstance.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModuleHelpers.h"
#include "PyNumbers.h"
#include "PyRuntime.h"
#include "PyStr.h"
#include "PyType.h"
#include "PyTypes.h"
#include "TopExceptionScope.h"
#include <errno.h>
#include <wtf/HexNumber.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The built-in exceptions. (Objects/exceptions.c in CPython is the reference.)

PY_DEFINE_CELL(PyException);

PyException::PyException(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyException* PyException::create(VM& vm, Structure* structure, PyType* type)
{
    auto* exception = new (NotNull, allocateCell<PyException>(vm)) PyException(vm, structure, type);
    exception->finishCreation(vm);
    return exception;
}

JSValue PyException::getPrototype(JSObject* object, JSGlobalObject*)
{
    auto* exception = uncheckedDowncast<PyException>(object);
    Runtime& rt = runtime(exception->realm());
    if (exception->type()->isSubtypeOf(rt.TypeError()))
        return rt.jsViewObject(JSViewObject::TypeErrorPrototype);
    return exception->getPrototypeDirect();
}

template<typename Visitor>
void PyException::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyException>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_args);
    visitor.append(thisObject->m_traceback);
    visitor.append(thisObject->m_cause);
    visitor.append(thisObject->m_context);
    visitor.append(thisObject->m_dict);
    visitor.appendValues(thisObject->m_fields.data(), numberOfFields);
}

DEFINE_VISIT_CHILDREN(PyException);

// ---- Making them from C++ ----

PyException* newException(JSGlobalObject* globalObject, PyType* type, std::span<const JSValue> args)
{
    // A RecursionError is made where the stack is as good as used up. A call would find that out in its turn, and raise another.
    // Calling the type runs nothing of the program's, so this is what comes of it.
    Runtime& rt = runtime(globalObject);
    if (type == rt.RecursionError()) [[unlikely]] {
        PyException* exception = PyException::create(rt.vm(), rt.structure<PyException>(), type);
        exception->setArgs(rt.vm(), PyTuple::create(globalObject, args));
        return exception;
    }
    JSValue result = call(globalObject, type, CallArgs(args));
    return result ? PyException::from(result) : nullptr;
}

PyException* newException(JSGlobalObject* globalObject, PyType* type, const String& message)
{
    JSValue argument = makeStr(globalObject, message);
    return newException(globalObject, type, std::span<const JSValue> { &argument, 1 });
}

// Of an object of JS. (0 is taken: see PyJSView.cpp.)
static constexpr unsigned jsErrorHiddenField = 1;
// The frames of Python it has passed through before there was a JSError to have the traceback.
static constexpr unsigned jsErrorTracebackHiddenField = 2;

// An object of JS keeps its exception. (Not one of Python's: the hidden fields of those belong to their types.)
static bool keepsItsException(Runtime& rt, JSValue thrown)
{
    PyType* type = rt.typeOf(thrown);
    return type == rt.jsObjectType() || type == rt.jsPlainObjectType() || type == rt.jsFunctionType();
}

// `object instanceof` the class whose prototype it is, without running code.
static bool inheritsFromPrototypeOfJS(JSObject* object, JSObject* wanted)
{
    for (JSValue prototype = object->getPrototypeDirect(); prototype.isObject(); prototype = asObject(prototype)->getPrototypeDirect()) {
        if (asObject(prototype) == wanted)
            return true;
    }
    return false;
}

static bool isTypeErrorOfJS(JSObject* thrown)
{
    return inheritsFromPrototypeOfJS(thrown, thrown->realm()->errorStructure(ErrorType::TypeError)->storedPrototypeObject());
}

static bool isErrorOfJS(JSValue value)
{
    return value.isObject() && !isPyObject(value) && inheritsFromPrototypeOfJS(asObject(value), asObject(value)->realm()->errorPrototype());
}

// class JSTypeError(JSError, TypeError): what stands for a TypeError of JS, so that `except TypeError` catches it. So for OSError
// and the classes derived from it. They are made when they are first asked for. JSError: it cannot be made.
PyType* jsErrorTypeFor(JSGlobalObject* globalObject, PyType* base)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!rt.slot(Runtime::Slot::JSErrorClasses))
        rt.setSlot(Runtime::Slot::JSErrorClasses, PyDict::create(globalObject));
    PyDict* classes = PyDict::from(rt.slot(Runtime::Slot::JSErrorClasses));
    if (JSValue type = classes->get(base->nameString()))
        return PyType::from(type);
    PyDict* ns = PyDict::create(globalObject);
    ns->set(vm, rt.names.dModule(), internString(vm, "_js"_s));
    JSValue type = call(globalObject, rt.typeType(), makeStr(vm, makeString("JS"_s, base->name())), PyTuple::create(globalObject, { rt.JSError(), base }), ns);
    // There may be next to no stack left, or memory.
    if (scope.exception()) [[unlikely]] {
        (void)scope.tryClearException();
        return rt.JSError();
    }
    classes->set(vm, base->nameString(), type);
    return PyType::from(type);
}

PyType* jsTypeErrorType(JSGlobalObject* globalObject)
{
    return jsErrorTypeFor(globalObject, runtime(globalObject).TypeError());
}

static PyType* osErrorSubclassFor(Runtime&, int errorNumber);

// An error of the system, as Node and Bun throw them, has the name of the error number as its `code` (and the number, negated or libuv's
// own, as its `errno`). In Python that is an OSError, of the class that OSError(errno, ...) makes. Null: it is no such error.
static PyException* osErrorFromThrownValue(JSGlobalObject* globalObject, JSObject* thrown)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!isErrorOfJS(thrown))
        return nullptr;
    auto property = [&](ASCIILiteral name) {
        JSValue value = thrown->get(globalObject, Identifier::fromString(vm, name));
        if (scope.exception()) [[unlikely]] {
            (void)scope.tryClearException();
            return JSValue();
        }
        return value;
    };
    JSValue number = property("errno"_s);
    JSValue code = number && number.isNumber() ? property("code"_s) : JSValue();
    if (!code || !code.isString())
        return nullptr;
    auto errorNumber = errnoForName(asString(code)->view(globalObject));
    if (!errorNumber)
        return nullptr;
    JSValue path = property("path"_s);
    JSValue destination = property("dest"_s);
    PyType* base = osErrorSubclassFor(rt, *errorNumber);
    bool hasPath = path && path.isString();
    bool hasDestination = hasPath && destination && destination.isString();
    JSValue arguments[] = { jsNumber(*errorNumber), makeStr(vm, String::fromUTF8(strerror(*errorNumber))), path, jsUndefined(), destination };
    PyType* type = jsErrorTypeFor(globalObject, base ? base : rt.OSError());
    if (type == rt.JSError())
        return nullptr;
    PyException* exception = newException(globalObject, type, std::span<const JSValue> { arguments }.first(hasDestination ? 5 : hasPath ? 3 : 2));
    if (scope.exception()) [[unlikely]]
        (void)scope.tryClearException();
    return exception;
}

JSValue exceptionFromThrownValue(JSGlobalObject* globalObject, JSValue thrown)
{
    if (PyException::is(thrown))
        return thrown;

    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    bool remembers = keepsItsException(rt, thrown);
    if (remembers) {
        if (JSValue existing = hiddenField(globalObject, thrown, jsErrorHiddenField))
            return existing;
    }
    PyException* exception;
    auto* error = dynamicDowncast<ErrorInstance>(thrown);
    if (error && error->isStackOverflowError())
        exception = newException(globalObject, rt.RecursionError(), "maximum recursion depth exceeded"_s);
    else if (error && error->isOutOfMemoryError())
        exception = newException(globalObject, rt.MemoryError(), std::span<const JSValue> { });
    else {
        exception = thrown.isObject() ? osErrorFromThrownValue(globalObject, asObject(thrown)) : nullptr;
        if (!exception) {
            String message = thrown.toWTFString(globalObject);
            if (scope.exception()) {
                scope.clearException();
                message = "<exception>"_s;
            }
            exception = newException(globalObject, thrown.isObject() && isTypeErrorOfJS(asObject(thrown)) ? jsTypeErrorType(globalObject) : rt.JSError(), message);
        }
        if (!failed(scope, exception))
            exception->setField(vm, ExceptionField::jsErrorValue, thrown);
    }
    if (failed(scope, exception)) [[unlikely]]
        return { };
    if (remembers) {
        setHiddenField(globalObject, thrown, jsErrorHiddenField, exception);
        if (JSValue traceback = hiddenField(globalObject, thrown, jsErrorTracebackHiddenField))
            exception->setTraceback(vm, traceback);
        // new Error(message, { cause }) is `raise ... from cause`, if the cause is something that can be raised.
        JSValue cause = asObject(thrown)->getDirect(vm, vm.propertyNames->cause);
        if (cause && (PyException::is(cause) || isErrorOfJS(cause)) && vm.isSafeToRecurseSoft()) {
            if (JSValue causeInPython = exceptionFromThrownValue(globalObject, cause)) {
                exception->setCause(vm, causeInPython);
                exception->setSuppressContext(true);
            }
        }
    }
    return exception;
}

bool isClassOfJSToCatch(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    return value.isObject() && rt.typeOf(value) == rt.jsFunctionType() && isClassOfJS(value.asCell());
}

bool standsForInstanceOfJS(JSGlobalObject* globalObject, JSValue value, JSValue classOfJS)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // Who only looks runs nothing that could raise.
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto* exception = PyException::tryFrom(value);
    if (!exception || !exception->type()->isSubtypeOf(rt.JSError()))
        return false;
    JSValue thrown = exception->field(ExceptionField::jsErrorValue);
    if (!thrown || !thrown.isObject())
        return false;
    JSObject* function = asObject(classOfJS);
    while (auto* bound = dynamicDowncast<JSBoundFunction>(function))
        function = bound->targetFunction();
    // (A function makes its prototype when it is first asked for.)
    PropertySlot slot(function, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool found = function->methodTable()->getOwnPropertySlot(function, globalObject, vm.propertyNames->prototype, slot);
    scope.assertNoException();
    if (!found || !slot.isValue())
        return false;
    JSValue prototype = slot.getValue(globalObject, vm.propertyNames->prototype);
    return prototype.isObject() && inheritsFromPrototypeOfJS(asObject(thrown), asObject(prototype));
}

// `raise js.RangeError("x")`, `raise js.RangeError`: what stands for the error, which is what is thrown. Empty: it is no Error of JS and
// no class of them, or an exception.
static JSValue exceptionForRaisedErrorOfJS(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isClassOfJSToCatch(globalObject, value)) {
        JSValue prototype = asObject(value)->getDirect(vm, vm.propertyNames->prototype);
        if (!prototype || !prototype.isObject() || (asObject(prototype) != asObject(prototype)->realm()->errorPrototype() && !isErrorOfJS(prototype)))
            return { };
        value = call(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!isErrorOfJS(value))
        return { };
    RELEASE_AND_RETURN(scope, exceptionFromThrownValue(globalObject, value));
}

bool addTracebackEntryToThrownObject(JSGlobalObject* globalObject, JSValue thrown, const ScopedLambda<JSValue(JSValue next)>& makeEntry)
{
    if (!thrown.isObject())
        return false;
    Runtime& rt = runtime(globalObject);
    if (!keepsItsException(rt, thrown))
        return false;
    if (JSValue entry = makeEntry(hiddenField(globalObject, thrown, jsErrorTracebackHiddenField)))
        setHiddenField(globalObject, thrown, jsErrorTracebackHiddenField, entry);
    return true;
}

PyException* exceptionOfThrownValueIfKnown(JSGlobalObject* globalObject, JSValue thrown)
{
    if (auto* exception = PyException::tryFrom(thrown))
        return exception;
    if (!thrown.isObject())
        return nullptr;
    Runtime& rt = runtime(globalObject);
    if (!keepsItsException(rt, thrown))
        return nullptr;
    return PyException::tryFrom(hiddenField(globalObject, thrown, jsErrorHiddenField));
}

// ---- The raise statement ----

void raiseStatement(JSGlobalObject* globalObject, JSValue exception, JSValue cause)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!exception) {
        // Again, what is being handled.
        JSValue handled = rt.handledException();
        if (!handled) {
            raiseRuntimeError(globalObject, scope, "No active exception to reraise"_s);
            return;
        }
        throwPythonException(globalObject, scope, handled);
        return;
    }

    auto isExceptionClass = [](JSValue value) {
        PyType* type = PyType::tryFrom(value);
        return type && type->has(TypeFlag::ExceptionSubclass);
    };

    JSValue value;
    if (isExceptionClass(exception)) {
        value = call(globalObject, exception);
        RETURN_IF_EXCEPTION(scope, void());
        if (!PyException::is(value)) {
            raiseTypeError(globalObject, scope, "calling "_s, reprForMessage(globalObject, exception), " should have returned an instance of BaseException, not "_s, reprForMessage(globalObject, rt.typeOf(value)));
            return;
        }
    } else if (PyException::is(exception))
        value = exception;
    else {
        value = exceptionForRaisedErrorOfJS(globalObject, exception);
        RETURN_IF_EXCEPTION(scope, void());
        if (!value) {
            raiseTypeError(globalObject, scope, "exceptions must derive from BaseException"_s);
            return;
        }
    }

    if (cause) {
        JSValue fixedCause;
        if (isExceptionClass(cause)) {
            fixedCause = call(globalObject, cause);
            RETURN_IF_EXCEPTION(scope, void());
            if (!PyException::is(fixedCause)) {
                raiseTypeError(globalObject, scope, "calling "_s, reprForMessage(globalObject, cause), " should have returned an instance of BaseException, not "_s, reprForMessage(globalObject, rt.typeOf(fixedCause)));
                return;
            }
        } else if (PyException::is(cause))
            fixedCause = cause;
        else if (!isNone(cause)) {
            fixedCause = exceptionForRaisedErrorOfJS(globalObject, cause);
            RETURN_IF_EXCEPTION(scope, void());
            if (!fixedCause) {
                raiseTypeError(globalObject, scope, "exception causes must derive from BaseException"_s);
                return;
            }
        }
        PyException::from(value)->setCause(vm, fixedCause);
        PyException::from(value)->setSuppressContext(true);
    }
    raiseException(globalObject, scope, value);
}

// ---- BaseException ----

static PyException* self(JSValue value) { return PyException::from(value); }

static JSValue noneIfEmpty(JSValue value) { return value ? value : jsUndefined(); }

static JSValue baseExceptionNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyException* exception = PyException::create(vm, rt.structure<PyException>(), PyType::from(cls));
    exception->setArgs(vm, PyTuple::create(globalObject, args.positional()));
    return exception;
}

static bool rejectKeywords(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    if (!args.hasKeywords())
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, typeName(globalObject, object), "() takes no keyword arguments"_s);
    return false;
}

static JSValue baseExceptionInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, object, args)))
        return { };
    self(object)->setArgs(getVM(globalObject), PyTuple::create(globalObject, args.positional()));
    return jsUndefined();
}

static JSValue baseExceptionStr(JSGlobalObject* globalObject, JSValue object)
{
    PyTuple* args = self(object)->args();
    switch (args->length()) {
    case 0:
        return runtime(globalObject).names.empty();
    case 1:
        return strObject(globalObject, args->at(0));
    default:
        return str(globalObject, args);
    }
}

static JSValue baseExceptionRepr(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* args = self(object)->args();
    // Without the module, even where messages have it: "Error('x')" for a binascii.Error.
    String name = typeOf(globalObject, object)->name();
    if (args->length() == 1) {
        JSString* argument = repr(globalObject, args->at(0));
        RETURN_IF_EXCEPTION(scope, { });
        String text = argument->value(globalObject);
        return makeStr(vm, makeString(name, '(', text, ')'));
    }
    JSString* arguments = repr(globalObject, args);
    RETURN_IF_EXCEPTION(scope, { });
    String text = arguments->value(globalObject);
    return makeStr(vm, makeString(name, text));
}

static JSValue baseExceptionReduce(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    if (exception->dict())
        return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()), JSValue(exception->dict()) });
    return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()) });
}

static JSValue baseExceptionSetState(JSGlobalObject* globalObject, JSValue object, JSValue state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(state))
        return jsUndefined();
    auto* dict = PyDict::tryFrom(state);
    if (!dict)
        return raiseTypeError(globalObject, scope, "state is not a dictionary"_s);
    JSValue key, value;
    for (unsigned position = 0; dict->next(position, key, value);) {
        if (!isStr(key))
            return raiseTypeError(globalObject, scope, "attribute name must be string, not '"_s, typeName(globalObject, key), '\'');
        setAttr(globalObject, object, strValue(key), value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static void setTraceback(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "__traceback__ may not be deleted"_s);
        return;
    }
    if (isNone(value)) {
        self(object)->setTraceback(vm, { });
        return;
    }
    if (!PyTraceback::is(value)) {
        raiseTypeError(globalObject, scope, "__traceback__ must be a traceback or None"_s);
        return;
    }
    self(object)->setTraceback(vm, value);
}

static JSValue baseExceptionWithTraceback(JSGlobalObject* globalObject, JSValue object, JSValue traceback)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    setTraceback(globalObject, object, traceback);
    RETURN_IF_EXCEPTION(scope, { });
    return object;
}

static JSValue baseExceptionAddNote(JSGlobalObject* globalObject, JSValue object, JSValue note)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(note))
        return raiseTypeError(globalObject, scope, "add_note() argument must be str, not "_s, isNone(note) ? String("None"_s) : typeName(globalObject, note));
    JSValue notes = getAttrIfPresent(globalObject, object, rt.names.dNotes());
    RETURN_IF_EXCEPTION(scope, { });
    if (!notes) {
        notes = newList(globalObject);
        setAttr(globalObject, object, rt.names.dNotes(), notes);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (!isList(notes))
        return raiseTypeError(globalObject, scope, "Cannot add note: __notes__ is not a list"_s);
    listAppend(globalObject, asList(notes), note);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue getArgs(JSGlobalObject*, JSValue object) { return self(object)->args(); }

static void setArgs(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "args may not be deleted"_s);
        return;
    }
    PyTuple* tuple = tupleFromIterable(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    self(object)->setArgs(vm, tuple);
}

static JSValue getTraceback(JSGlobalObject*, JSValue object) { return noneIfEmpty(self(object)->traceback()); }
static JSValue getContext(JSGlobalObject*, JSValue object) { return noneIfEmpty(self(object)->context()); }
static JSValue getCause(JSGlobalObject*, JSValue object) { return noneIfEmpty(self(object)->cause()); }
static JSValue getSuppressContext(JSGlobalObject*, JSValue object) { return jsBoolean(self(object)->suppressContext()); }

static bool checkExceptionOrNone(JSGlobalObject* globalObject, JSValue& value, ASCIILiteral what, ASCIILiteral description)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, what, " may not be deleted"_s);
        return false;
    }
    if (isNone(value)) {
        value = JSValue();
        return true;
    }
    if (!PyException::is(value)) {
        raiseTypeError(globalObject, scope, "exception "_s, description, " must be None or derive from BaseException"_s);
        return false;
    }
    return true;
}

static void setContext(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    if (checkExceptionOrNone(globalObject, value, "__context__"_s, "context"_s))
        self(object)->setContext(getVM(globalObject), value);
}

static void setCause(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    if (checkExceptionOrNone(globalObject, value, "__cause__"_s, "cause"_s)) {
        self(object)->setCause(getVM(globalObject), value);
        self(object)->setSuppressContext(true);
    }
}

static void setSuppressContext(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !value.isBoolean()) {
        raiseTypeError(globalObject, scope, value ? "attribute value type must be bool"_s : "can't delete numeric/char attribute"_s);
        return;
    }
    self(object)->setSuppressContext(value.asBoolean());
}

// The attributes that are kept in the fields.
template<unsigned index>
static JSValue getField(JSGlobalObject*, JSValue object) { return noneIfEmpty(self(object)->field(index)); }

template<unsigned index>
static void setField(JSGlobalObject* globalObject, JSValue object, JSValue value) { self(object)->setField(getVM(globalObject), index, value); }

#define PY_EXCEPTION_FIELD(name, index) NativeGetSet { name ""_s, getField<ExceptionField::index>, setField<ExceptionField::index>, true }

static constexpr NativeDef baseExceptionMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", baseExceptionInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", baseExceptionStr, "($self, /)"),
    PY_SLOT("__repr__", baseExceptionRepr, "($self, /)"),
    PY_METHOD("__reduce__", baseExceptionReduce, "($self, /)"),
    PY_METHOD("__setstate__", baseExceptionSetState, "($self, state, /)"),
    PY_METHOD("with_traceback", baseExceptionWithTraceback, "($self, tb, /)"),
    PY_METHOD("add_note", baseExceptionAddNote, "($self, note, /)"),
};

static constexpr NativeGetSet baseExceptionGetSets[] = {
    { "args"_s, getArgs, setArgs },
    { "__traceback__"_s, getTraceback, setTraceback },
    { "__context__"_s, getContext, setContext },
    { "__cause__"_s, getCause, setCause },
    PY_WRITABLE_MEMBER("__suppress_context__", getSuppressContext, setSuppressContext),
};

// ---- StopIteration, SystemExit ----

static JSValue stopIterationInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    if (!baseExceptionInit(globalObject, object, args))
        return { };
    self(object)->setField(getVM(globalObject), ExceptionField::stopIterationValue, args.size() ? args[0] : jsUndefined());
    return jsUndefined();
}

static constexpr NativeDef stopIterationMethods[] = {
    PY_SLOT("__init__", stopIterationInit, "($self, *args, **kwargs)"),
};
static constexpr NativeGetSet stopIterationGetSets[] = {
    PY_EXCEPTION_FIELD("value", stopIterationValue),
};

static JSValue systemExitInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    if (!baseExceptionInit(globalObject, object, args))
        return { };
    JSValue code = !args.size() ? jsUndefined() : args.size() == 1 ? args[0] : JSValue(self(object)->args());
    self(object)->setField(getVM(globalObject), ExceptionField::systemExitCode, code);
    return jsUndefined();
}

static constexpr NativeDef systemExitMethods[] = {
    PY_SLOT("__init__", systemExitInit, "($self, *args, **kwargs)"),
};
static constexpr NativeGetSet systemExitGetSets[] = {
    PY_EXCEPTION_FIELD("code", systemExitCode),
};

// ---- Those that take keyword arguments ----

// False: an exception. Each keyword goes in the field that goes with its name.
static bool initWithKeywords(JSGlobalObject* globalObject, ASCIILiteral className, JSValue object, const CallArgs& args, std::initializer_list<std::pair<ASCIILiteral, unsigned>> keywords)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    if (args.keywordCount() > keywords.size()) {
        raiseTypeError(globalObject, scope, className, "() takes at most "_s, keywords.size(), " keyword argument"_s, keywords.size() == 1 ? ""_s : "s"_s, " ("_s, args.keywordCount(), " given)"_s);
        return false;
    }
    exception->setArgs(vm, PyTuple::create(globalObject, args.positional()));
    for (auto& keyword : keywords)
        exception->setField(vm, keyword.second, { });
    if (args.keywordCount() > keywords.size()) {
        raiseTypeError(globalObject, scope, className, "() takes at most "_s, static_cast<unsigned>(keywords.size()), " keyword argument"_s, keywords.size() == 1 ? ""_s : "s"_s, " ("_s, args.keywordCount(), " given)"_s);
        return false;
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        StringImpl* name = args.keywordName(i)->tryGetValueImpl();
        bool found = false;
        for (auto& keyword : keywords) {
            if (WTF::equal(name, keyword.first.span8())) {
                exception->setField(vm, keyword.second, args.keywordValue(i));
                found = true;
                break;
            }
        }
        if (!found) {
            Vector<ASCIILiteral, 4> names;
            for (auto& keyword : keywords)
                names.append(keyword.first);
            scope.release();
            raiseUnexpectedKeyword(globalObject, className, String(name), names.span());
            return false;
        }
    }
    return true;
}

static JSValue importErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, initWithKeywords(globalObject, "ImportError"_s, object, args, { { "name"_s, ExceptionField::importErrorName }, { "path"_s, ExceptionField::importErrorPath }, { "name_from"_s, ExceptionField::importErrorNameFrom } })))
        return { };
    self(object)->setField(getVM(globalObject), ExceptionField::importErrorMsg, args.size() == 1 ? args[0] : jsUndefined());
    return jsUndefined();
}

static JSValue importErrorStr(JSGlobalObject* globalObject, JSValue object)
{
    JSValue message = self(object)->field(ExceptionField::importErrorMsg);
    if (message && isStr(message))
        return strValue(message);
    return baseExceptionStr(globalObject, object);
}

// ImportError('message', name='module', path='file')
static JSValue importErrorRepr(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    JSValue base = baseExceptionRepr(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue name = exception->field(ExceptionField::importErrorName);
    JSValue path = exception->field(ExceptionField::importErrorPath);
    if (!name && !path)
        return base;
    String text = asString(base)->value(globalObject);
    StrBuilder builder;
    builder.append(StringView(text).left(text.length() - 1));
    bool needsComma = !!exception->args()->length();
    auto append = [&](ASCIILiteral keyword, JSValue value) {
        if (!value)
            return true;
        JSString* string = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        builder.append(needsComma ? ", "_s : ""_s, keyword, '=', String(string->value(globalObject)));
        needsComma = true;
        return true;
    };
    if (!append("name"_s, name) || !append("path"_s, path))
        return { };
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// The __dict__ and, under their names, the fields that are set. None: there is neither.
static JSValue stateWithFields(JSGlobalObject* globalObject, PyException* exception, std::initializer_list<std::pair<ASCIILiteral, JSValue>> fields)
{
    VM& vm = getVM(globalObject);
    PyDict* state = nullptr;
    for (auto& field : fields) {
        if (!field.second)
            continue;
        if (!state)
            state = exception->dict() ? exception->dict()->copy(globalObject) : PyDict::create(globalObject);
        state->set(vm, internString(vm, field.first), field.second);
    }
    if (state)
        return state;
    return exception->dict() ? JSValue(exception->dict()) : jsUndefined();
}

static JSValue importErrorReduce(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    JSValue state = stateWithFields(globalObject, exception, { { "name"_s, exception->field(ExceptionField::importErrorName) }, { "path"_s, exception->field(ExceptionField::importErrorPath) }, { "name_from"_s, exception->field(ExceptionField::importErrorNameFrom) } });
    if (isNone(state))
        return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()) });
    return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()), state });
}

static constexpr NativeDef importErrorMethods[] = {
    PY_SLOT("__init__", importErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", importErrorRepr, "($self, /)"),
    PY_SLOT("__str__", importErrorStr, "($self, /)"),
    PY_METHOD("__reduce__", importErrorReduce, "($self, /)"),
};
static constexpr NativeGetSet importErrorGetSets[] = {
    PY_EXCEPTION_FIELD("msg", importErrorMsg),
    PY_EXCEPTION_FIELD("name", importErrorName),
    PY_EXCEPTION_FIELD("path", importErrorPath),
    PY_EXCEPTION_FIELD("name_from", importErrorNameFrom),
};

static JSValue nameErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, initWithKeywords(globalObject, "NameError"_s, object, args, { { "name"_s, ExceptionField::nameErrorName } })))
        return { };
    return jsUndefined();
}

static constexpr NativeDef nameErrorMethods[] = {
    PY_SLOT("__init__", nameErrorInit, "($self, *args, **kwargs)"),
};
static constexpr NativeGetSet nameErrorGetSets[] = {
    PY_EXCEPTION_FIELD("name", nameErrorName),
};

static JSValue attributeErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, initWithKeywords(globalObject, "AttributeError"_s, object, args, { { "name"_s, ExceptionField::attributeErrorName }, { "obj"_s, ExceptionField::attributeErrorObj } })))
        return { };
    return jsUndefined();
}

// Without obj, which often cannot be pickled.
static JSValue attributeErrorGetState(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    return stateWithFields(globalObject, exception, { { "name"_s, exception->field(ExceptionField::attributeErrorName) }, { "args"_s, exception->args() } });
}

static JSValue attributeErrorReduce(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(exception->args()), attributeErrorGetState(globalObject, object) });
}

static constexpr NativeDef attributeErrorMethods[] = {
    PY_SLOT("__init__", attributeErrorInit, "($self, *args, **kwargs)"),
    PY_METHOD("__getstate__", attributeErrorGetState, "($self, /)"),
    PY_METHOD("__reduce__", attributeErrorReduce, "($self, /)"),
};
static constexpr NativeGetSet attributeErrorGetSets[] = {
    PY_EXCEPTION_FIELD("name", attributeErrorName),
    PY_EXCEPTION_FIELD("obj", attributeErrorObj),
};

// ---- KeyError ----

static JSValue keyErrorStr(JSGlobalObject* globalObject, JSValue object)
{
    // KeyError('') would otherwise show as nothing at all.
    PyTuple* args = self(object)->args();
    if (args->length() == 1)
        return reprObject(globalObject, args->at(0));
    return baseExceptionStr(globalObject, object);
}

static constexpr NativeDef keyErrorMethods[] = {
    PY_SLOT("__str__", keyErrorStr, "($self, /)"),
};

// ---- OSError ----

static PyType* osErrorSubclassFor(Runtime& rt, int errorNumber)
{
    switch (errorNumber) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EALREADY:
    case EINPROGRESS:
        return rt.BlockingIOError();
    case EPIPE:
    case ESHUTDOWN:
        return rt.BrokenPipeError();
    case ECHILD:
        return rt.ChildProcessError();
    case ECONNABORTED:
        return rt.ConnectionAbortedError();
    case ECONNREFUSED:
        return rt.ConnectionRefusedError();
    case ECONNRESET:
        return rt.ConnectionResetError();
    case EEXIST:
        return rt.FileExistsError();
    case ENOENT:
        return rt.FileNotFoundError();
    case EISDIR:
        return rt.IsADirectoryError();
    case ENOTDIR:
        return rt.NotADirectoryError();
    case EINTR:
        return rt.InterruptedError();
    case EACCES:
    case EPERM:
        return rt.PermissionError();
    case ESRCH:
        return rt.ProcessLookupError();
    case ETIMEDOUT:
        return rt.TimeoutError();
    default:
        return nullptr;
    }
}

// False: an exception.
static bool osErrorInitialize(JSGlobalObject* globalObject, PyException* exception, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = args.size();
    std::span<const JSValue> kept = args.positional();
    if (count >= 2 && count <= 5) {
        exception->setField(vm, ExceptionField::osErrorErrno, args[0]);
        exception->setField(vm, ExceptionField::osErrorStrerror, args[1]);
        if (count >= 3) {
            JSValue filename = args[2];
            PyType* filenameType = rt.typeOf(filename);
            bool isNumber = filenameType->lookup(Special::Index) || filenameType->lookup(Special::Int) || filenameType->lookup(Special::Float);
            if (exception->type() == rt.BlockingIOError() && isNumber) {
                // In place of a file name, how many characters were written. Nothing is then taken out of the args.
                int64_t written = toSsize(globalObject, filename, rt.ValueError());
                RETURN_IF_EXCEPTION(scope, false);
                // -1 is CPython's mark for "not set".
                if (written != -1)
                    exception->setField(vm, ExceptionField::osErrorCharactersWritten, makeInt(globalObject, written));
            } else if (!isNone(filename)) {
                exception->setField(vm, ExceptionField::osErrorFilename, filename);
                if (count == 5 && !isNone(args[4]))
                    exception->setField(vm, ExceptionField::osErrorFilename2, args[4]);
                // The file names are not among the args, for compatibility.
                kept = kept.first(2);
            }
        }
    }
    exception->setArgs(vm, PyTuple::create(globalObject, kept));
    return true;
}

// A subclass that has an __init__ of its own, and not a __new__, gets to see the arguments first.
static bool osErrorUsesInit(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    return type->lookup(Special::Init) != rt.OSError()->lookup(Special::Init) && type->lookup(Special::New) == rt.OSError()->lookup(Special::New);
}

static JSValue osErrorNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    bool usesInit = osErrorUsesInit(globalObject, type);
    if (!usesInit) {
        if (args.hasKeywords())
            return raiseTypeError(globalObject, scope, type->name(), "() takes no keyword arguments"_s);
        if (type == rt.OSError() && args.size() >= 2 && args.size() <= 5 && isInt(args[0])) {
            auto number = tryInt64(intValue(args[0]));
            if (PyType* subclass = number && *number == static_cast<int>(*number) ? osErrorSubclassFor(rt, static_cast<int>(*number)) : nullptr)
                type = subclass;
        }
    }
    PyException* exception = PyException::create(vm, rt.structure<PyException>(), type);
    if (usesInit)
        exception->setArgs(vm, rt.emptyTuple());
    else if (failed(scope, osErrorInitialize(globalObject, exception, args)))
        return { };
    return exception;
}

static JSValue osErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!osErrorUsesInit(globalObject, self(object)->type()))
        return jsUndefined();
    if (failed(scope, rejectKeywords(globalObject, object, args)))
        return { };
    if (failed(scope, osErrorInitialize(globalObject, self(object), args)))
        return { };
    return jsUndefined();
}

static JSValue osErrorStr(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    JSValue errorNumber = exception->field(ExceptionField::osErrorErrno);
    JSValue message = exception->field(ExceptionField::osErrorStrerror);
    JSValue filename = exception->field(ExceptionField::osErrorFilename);
    JSValue filename2 = exception->field(ExceptionField::osErrorFilename2);

    auto text = [&](JSValue value, bool useRepr) -> String {
        JSString* string = useRepr ? repr(globalObject, noneIfEmpty(value)) : str(globalObject, noneIfEmpty(value));
        if (!string)
            return { };
        return string->value(globalObject);
    };
    if (filename) {
        String first = text(errorNumber, false);
        RETURN_IF_EXCEPTION(scope, { });
        String second = text(message, false);
        RETURN_IF_EXCEPTION(scope, { });
        String third = text(filename, true);
        RETURN_IF_EXCEPTION(scope, { });
        if (filename2) {
            String fourth = text(filename2, true);
            RETURN_IF_EXCEPTION(scope, { });
            return makeStr(vm, makeString("[Errno "_s, first, "] "_s, second, ": "_s, third, " -> "_s, fourth));
        }
        return makeStr(vm, makeString("[Errno "_s, first, "] "_s, second, ": "_s, third));
    }
    if (errorNumber && message) {
        String first = text(errorNumber, false);
        RETURN_IF_EXCEPTION(scope, { });
        String second = text(message, false);
        RETURN_IF_EXCEPTION(scope, { });
        return makeStr(vm, makeString("[Errno "_s, first, "] "_s, second));
    }
    RELEASE_AND_RETURN(scope, baseExceptionStr(globalObject, object));
}

static JSValue osErrorReduce(JSGlobalObject* globalObject, JSValue object)
{
    PyException* exception = self(object);
    PyTuple* args = exception->args();
    JSValue filename = exception->field(ExceptionField::osErrorFilename);
    if (args->length() == 2 && filename) {
        JSValue filename2 = exception->field(ExceptionField::osErrorFilename2);
        if (filename2)
            args = PyTuple::create(globalObject, { args->at(0), args->at(1), filename, jsUndefined(), filename2 });
        else
            args = PyTuple::create(globalObject, { args->at(0), args->at(1), filename });
    }
    if (exception->dict())
        return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(args), JSValue(exception->dict()) });
    return PyTuple::create(globalObject, { JSValue(exception->type()), JSValue(args) });
}

static JSValue getCharactersWritten(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = self(object)->field(ExceptionField::osErrorCharactersWritten);
    if (!value)
        return raise(globalObject, scope, runtime(globalObject).AttributeError(), "characters_written"_s);
    return value;
}

static void setCharactersWritten(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    if (!value) {
        if (!exception->field(ExceptionField::osErrorCharactersWritten)) {
            raise(globalObject, scope, rt.AttributeError(), "characters_written"_s);
            return;
        }
        exception->setField(vm, ExceptionField::osErrorCharactersWritten, { });
        return;
    }
    int64_t count = toSsize(globalObject, value, rt.ValueError());
    RETURN_IF_EXCEPTION(scope, void());
    // -1 is how CPython says that there is none.
    exception->setField(vm, ExceptionField::osErrorCharactersWritten, count == -1 ? JSValue() : makeInt(globalObject, count));
}

static constexpr NativeDef osErrorMethods[] = {
    PY_NEW(osErrorNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", osErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", osErrorStr, "($self, /)"),
    PY_METHOD("__reduce__", osErrorReduce, "($self, /)"),
};
static constexpr NativeGetSet osErrorGetSets[] = {
    PY_EXCEPTION_FIELD("errno", osErrorErrno),
    PY_EXCEPTION_FIELD("strerror", osErrorStrerror),
    PY_EXCEPTION_FIELD("filename", osErrorFilename),
    PY_EXCEPTION_FIELD("filename2", osErrorFilename2),
    { "characters_written"_s, getCharactersWritten, setCharactersWritten },
};

// ---- SyntaxError ----

static JSValue syntaxErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, baseExceptionInit(globalObject, object, args)))
        return { };
    PyException* exception = self(object);
    if (args.size() >= 1)
        exception->setField(vm, ExceptionField::syntaxErrorMsg, args[0]);
    if (args.size() == 2) {
        PyTuple* info = tupleFromIterable(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        unsigned length = info->length();
        if (length < 4 || length > 7)
            return raiseTypeError(globalObject, scope, "function takes "_s, length < 4 ? "at least 4"_s : "at most 7"_s, " arguments ("_s, length, " given)"_s);
        if (length == 5)
            return raiseTypeError(globalObject, scope, "end_offset must be provided when end_lineno is provided"_s);
        exception->setField(vm, ExceptionField::syntaxErrorFilename, info->at(0));
        exception->setField(vm, ExceptionField::syntaxErrorLineno, info->at(1));
        exception->setField(vm, ExceptionField::syntaxErrorOffset, info->at(2));
        exception->setField(vm, ExceptionField::syntaxErrorText, info->at(3));
        exception->setField(vm, ExceptionField::syntaxErrorEndLineno, length >= 6 ? info->at(4) : JSValue());
        exception->setField(vm, ExceptionField::syntaxErrorEndOffset, length >= 6 ? info->at(5) : JSValue());
        exception->setField(vm, ExceptionField::syntaxErrorMetadata, length == 7 ? info->at(6) : JSValue());
    }
    return jsUndefined();
}

static JSValue syntaxErrorStr(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    JSString* message = str(globalObject, noneIfEmpty(exception->field(ExceptionField::syntaxErrorMsg)));
    RETURN_IF_EXCEPTION(scope, { });

    JSValue filename = exception->field(ExceptionField::syntaxErrorFilename);
    JSValue lineNumber = exception->field(ExceptionField::syntaxErrorLineno);
    bool hasFilename = filename && isStr(filename);
    bool hasLineNumber = lineNumber && isExactInt(lineNumber);
    if (!hasFilename && !hasLineNumber)
        return message;

    String text = message->value(globalObject);
    String baseName;
    if (hasFilename) {
        baseName = strValue(filename)->value(globalObject);
        size_t slash = baseName.reverseFind('/');
        if (slash != notFound)
            baseName = baseName.substring(slash + 1);
    }
    // The line is shown as a number of C, which is -1 if it does not fit.
    String line = hasLineNumber ? (tryInt64(lineNumber) ? intToString(globalObject, lineNumber) : String("-1"_s)) : String();
    if (hasFilename && hasLineNumber)
        return makeStr(vm, makeString(text, " ("_s, baseName, ", line "_s, line, ')'));
    if (hasFilename)
        return makeStr(vm, makeString(text, " ("_s, baseName, ')'));
    return makeStr(vm, makeString(text, " (line "_s, line, ')'));
}

static constexpr NativeDef syntaxErrorMethods[] = {
    PY_SLOT("__init__", syntaxErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", syntaxErrorStr, "($self, /)"),
};
static constexpr NativeGetSet syntaxErrorGetSets[] = {
    PY_EXCEPTION_FIELD("msg", syntaxErrorMsg),
    PY_EXCEPTION_FIELD("filename", syntaxErrorFilename),
    PY_EXCEPTION_FIELD("lineno", syntaxErrorLineno),
    PY_EXCEPTION_FIELD("offset", syntaxErrorOffset),
    PY_EXCEPTION_FIELD("text", syntaxErrorText),
    PY_EXCEPTION_FIELD("end_lineno", syntaxErrorEndLineno),
    PY_EXCEPTION_FIELD("end_offset", syntaxErrorEndOffset),
    PY_EXCEPTION_FIELD("print_file_and_line", syntaxErrorPrintFileAndLine),
    PY_EXCEPTION_FIELD("_metadata", syntaxErrorMetadata),
};

// ---- UnicodeError ----

static JSValue unicodeErrorInitCommon(JSGlobalObject* globalObject, JSValue object, const CallArgs& args, bool hasEncoding, bool isDecoding)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, baseExceptionInit(globalObject, object, args)))
        return { };
    unsigned expected = hasEncoding ? 5 : 4;
    if (args.size() != expected)
        return raiseTypeError(globalObject, scope, "function takes exactly "_s, expected, " arguments ("_s, args.size(), " given)"_s);
    unsigned next = 0;
    auto takeStr = [&]() -> JSValue {
        JSValue value = args[next++];
        if (!isStr(value))
            return raiseTypeError(globalObject, scope, "argument "_s, next, " must be str, not "_s, isNone(value) ? String("None"_s) : typeName(globalObject, value));
        return value;
    };
    auto takeIndex = [&]() -> JSValue {
        int64_t value;
        if (failed(scope, ssizeArgument(globalObject, args[next++], value)))
            return { };
        return makeInt(globalObject, value);
    };
    JSValue encoding;
    if (hasEncoding) {
        encoding = takeStr();
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue target = isDecoding ? args[next++] : takeStr();
    RETURN_IF_EXCEPTION(scope, { });
    JSValue start = takeIndex();
    RETURN_IF_EXCEPTION(scope, { });
    JSValue end = takeIndex();
    RETURN_IF_EXCEPTION(scope, { });
    JSValue reason = takeStr();
    RETURN_IF_EXCEPTION(scope, { });
    if (isDecoding && !PyBytes::is(target)) {
        // What could change is copied.
        std::span<const uint8_t> bytes;
        if (failed(scope, getBytesLike(globalObject, target, bytes)))
            return { };
        target = PyBytes::create(globalObject, bytes);
    }
    PyException* exception = self(object);
    exception->setField(vm, ExceptionField::unicodeErrorEncoding, encoding);
    exception->setField(vm, ExceptionField::unicodeErrorObject, target);
    exception->setField(vm, ExceptionField::unicodeErrorStart, start);
    exception->setField(vm, ExceptionField::unicodeErrorEnd, end);
    exception->setField(vm, ExceptionField::unicodeErrorReason, reason);
    return jsUndefined();
}

static JSValue unicodeEncodeErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args) { return unicodeErrorInitCommon(globalObject, object, args, true, false); }
static JSValue unicodeDecodeErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args) { return unicodeErrorInitCommon(globalObject, object, args, true, true); }
static JSValue unicodeTranslateErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args) { return unicodeErrorInitCommon(globalObject, object, args, false, false); }

static String escapedCharacter(char32_t ch)
{
    if (ch <= 0xff)
        return makeString("\\x"_s, hex(ch, 2, Lowercase));
    if (ch <= 0xffff)
        return makeString("\\u"_s, hex(ch, 4, Lowercase));
    return makeString("\\U"_s, hex(ch, 8, Lowercase));
}

static JSValue unicodeErrorStrCommon(JSGlobalObject* globalObject, JSValue object, int kind)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    JSValue target = exception->field(ExceptionField::unicodeErrorObject);
    if (!target)
        RELEASE_AND_RETURN(scope, runtime(globalObject).names.empty());

    // What __init__ has not set shows as it does in CPython: null pointers and zeros.
    auto textOf = [&](unsigned field) -> String {
        JSValue value = exception->field(field);
        if (!value)
            return "<NULL>"_s;
        JSString* string = str(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        return string->value(globalObject);
    };
    auto positionOf = [&](unsigned field) -> int64_t {
        JSValue value = exception->field(field);
        return value ? toSsize(globalObject, value, nullptr) : 0;
    };
    String reason = textOf(ExceptionField::unicodeErrorReason);
    RETURN_IF_EXCEPTION(scope, { });
    String encoding;
    if (kind != 2) {
        encoding = textOf(ExceptionField::unicodeErrorEncoding);
        RETURN_IF_EXCEPTION(scope, { });
    }
    int64_t start = positionOf(ExceptionField::unicodeErrorStart);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t end = positionOf(ExceptionField::unicodeErrorEnd);
    RETURN_IF_EXCEPTION(scope, { });

    // A __str__ called above may have done anything to the exception.
    target = exception->field(ExceptionField::unicodeErrorObject);
    if (!target)
        return raiseTypeError(globalObject, scope, "UnicodeError 'object' attribute is not set"_s);
    if (kind == 1 ? !PyBytes::is(target) : !isStr(target))
        return raiseTypeError(globalObject, scope, "UnicodeError 'object' attribute must be a "_s, kind == 1 ? "bytes"_s : "string"_s);

    if (kind == 1) {
        std::span<const uint8_t> bytes;
        if (failed(scope, getBytesLike(globalObject, target, bytes)))
            return { };
        if (start >= 0 && start < static_cast<int64_t>(bytes.size()) && end == start + 1)
            return makeStr(vm, makeString('\'', encoding, "' codec can't decode byte 0x"_s, hex(bytes[start], 2, Lowercase), " in position "_s, start, ": "_s, reason));
        return makeStr(vm, makeString('\'', encoding, "' codec can't decode bytes in position "_s, start, '-', end - 1, ": "_s, reason));
    }

    ASCIILiteral verb = kind ? "translate"_s : "encode"_s;
    String prefix = kind ? String("can't "_s) : makeString('\'', encoding, "' codec can't "_s);
    if (isStr(target)) {
        StrView view(globalObject, strValue(target));
        RETURN_IF_EXCEPTION(scope, { });
        if (start >= 0 && start < view.length() && end == start + 1)
            return makeStr(vm, makeString(prefix, verb, " character '"_s, escapedCharacter(view.at(static_cast<unsigned>(start))), "' in position "_s, start, ": "_s, reason));
    }
    return makeStr(vm, makeString(prefix, verb, " characters in position "_s, start, '-', end - 1, ": "_s, reason));
}

static JSValue unicodeEncodeErrorStr(JSGlobalObject* globalObject, JSValue object) { return unicodeErrorStrCommon(globalObject, object, 0); }
static JSValue unicodeDecodeErrorStr(JSGlobalObject* globalObject, JSValue object) { return unicodeErrorStrCommon(globalObject, object, 1); }
static JSValue unicodeTranslateErrorStr(JSGlobalObject* globalObject, JSValue object) { return unicodeErrorStrCommon(globalObject, object, 2); }

static constexpr NativeDef unicodeEncodeErrorMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", unicodeEncodeErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", unicodeEncodeErrorStr, "($self, /)"),
};
static constexpr NativeDef unicodeDecodeErrorMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", unicodeDecodeErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", unicodeDecodeErrorStr, "($self, /)"),
};
static constexpr NativeDef unicodeTranslateErrorMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", unicodeTranslateErrorInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", unicodeTranslateErrorStr, "($self, /)"),
};
// start and end are numbers of C in CPython.
template<unsigned index>
static void setPositionField(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isInt(value)) {
        raiseTypeError(globalObject, scope, value ? "an integer is required"_s : "can't delete numeric/char attribute"_s);
        return;
    }
    int64_t position;
    if (failed(scope, ssizeArgument(globalObject, value, position)))
        return;
    self(object)->setField(vm, index, makeInt(globalObject, position));
}

static constexpr NativeGetSet unicodeErrorGetSets[] = {
    PY_EXCEPTION_FIELD("encoding", unicodeErrorEncoding),
    PY_EXCEPTION_FIELD("object", unicodeErrorObject),
    PY_WRITABLE_MEMBER("start", getField<ExceptionField::unicodeErrorStart>, setPositionField<ExceptionField::unicodeErrorStart>),
    PY_WRITABLE_MEMBER("end", getField<ExceptionField::unicodeErrorEnd>, setPositionField<ExceptionField::unicodeErrorEnd>),
    PY_EXCEPTION_FIELD("reason", unicodeErrorReason),
};

// ---- JSError: what JS code threw ----

static constexpr NativeGetSet jsErrorGetSets[] = {
    PY_EXCEPTION_FIELD("value", jsErrorValue),
};

// What the exception has not, the object that was thrown may have: error.code, error.message, error.cause.
static JSValue jsErrorGetAttr(JSGlobalObject* globalObject, JSValue object, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue thrown = self(object)->field(ExceptionField::jsErrorValue);
    if (thrown && thrown.isObject() && typeOf(globalObject, thrown)->isSubtypeOf(runtime(globalObject).jsObjectType())) {
        JSValue property = attributeFromPropertyOfJS(globalObject, asObject(thrown), name);
        RETURN_IF_EXCEPTION(scope, { });
        if (property)
            return property;
    }
    return raiseAttributeError(globalObject, scope, object, name, makeString('\'', typeName(globalObject, object), "' object has no attribute '"_s, stringOf(name), '\''));
}

// That is for Python. JS has what was thrown itself, error.value, and is shown the exception as any other: its class, its message.
JSValue attributeOfExceptionForJS(JSGlobalObject* globalObject, PyException* exception, JSString* name)
{
    auto isNative = [&](Special special, NativeDef::F2 function) {
        auto* native = PyNativeFunction::tryFrom(exception->type()->lookup(special));
        return native && native->def().pointer.f2 == function;
    };
    if (isNative(Special::GetAttr, jsErrorGetAttr) && isNative(Special::GetAttribute, objectGetAttribute))
        return genericGetAttr(globalObject, exception, name, true);
    return getAttrIfPresent(globalObject, exception, name);
}

static constexpr NativeDef jsErrorMethods[] = {
    PY_SLOT("__getattr__", jsErrorGetAttr, "($self, name, /)"),
};

// ---- The types ----

// In CPython every type whose instances are laid out as those of BaseException has a __new__ of its own.
static constexpr NativeDef plainExceptionMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
};

void initializeExceptionTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);

    static constexpr OptionSet<TypeFlag> flags { TypeFlag::Base, TypeFlag::HasDict };
    static constexpr OptionSet<TypeFlag> flagsWithFields { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::OwnLayout };

    auto define = [&](PyType* type, ASCIILiteral name, PyType* base) {
        TypeDef def { name, flags, { }, { }, { } };
        if (type == rt.BaseException()) {
            def.flags = flagsWithFields;
            def.flags.add(TypeFlag::ExceptionSubclass);
            def.methods = baseExceptionMethods;
            def.getsets = baseExceptionGetSets;
        } else if (type == rt.StopIteration()) {
            def.flags = flagsWithFields;
            def.methods = stopIterationMethods;
            def.getsets = stopIterationGetSets;
        } else if (type == rt.SystemExit()) {
            def.flags = flagsWithFields;
            def.methods = systemExitMethods;
            def.getsets = systemExitGetSets;
        } else if (type == rt.ImportError()) {
            def.flags = flagsWithFields;
            def.methods = importErrorMethods;
            def.getsets = importErrorGetSets;
        } else if (type == rt.NameError()) {
            def.flags = flagsWithFields;
            def.methods = nameErrorMethods;
            def.getsets = nameErrorGetSets;
        } else if (type == rt.AttributeError()) {
            def.flags = flagsWithFields;
            def.methods = attributeErrorMethods;
            def.getsets = attributeErrorGetSets;
        } else if (type == rt.KeyError())
            def.methods = keyErrorMethods;
        else if (type == rt.OSError()) {
            def.flags = flagsWithFields;
            def.methods = osErrorMethods;
            def.getsets = osErrorGetSets;
        } else if (type == rt.SyntaxError()) {
            def.flags = flagsWithFields;
            def.methods = syntaxErrorMethods;
            def.getsets = syntaxErrorGetSets;
        } else if (type == rt.UnicodeEncodeError()) {
            def.flags = flagsWithFields;
            def.methods = unicodeEncodeErrorMethods;
            def.getsets = unicodeErrorGetSets;
        } else if (type == rt.UnicodeDecodeError()) {
            def.flags = flagsWithFields;
            def.methods = unicodeDecodeErrorMethods;
            def.getsets = unicodeErrorGetSets;
        } else if (type == rt.UnicodeTranslateError()) {
            def.flags = flagsWithFields;
            def.methods = unicodeTranslateErrorMethods;
            def.getsets = unicodeErrorGetSets;
        } else if (type == rt.JSError()) {
            def.name = "_js.JSError"_s;
            def.methods = jsErrorMethods;
            def.getsets = jsErrorGetSets;
        } else if (type == rt.IncompleteInputError())
            def.name = "_IncompleteInputError"_s;
        else if (base->solidBase() == rt.BaseException() && type != rt.BaseExceptionGroup() && type != rt.ExceptionGroup())
            def.methods = plainExceptionMethods;
        type->initializeBuiltin(globalObject, def, rt.typeType(), base);
    };

#define PY_INITIALIZE_EXCEPTION(id, base) define(rt.id(), #id ""_s, exceptionBase_##base(rt));
#define PY_DEFINE_BASE_GETTER(id, base) [[maybe_unused]] auto exceptionBase_##id = [](Runtime& rt) { return rt.id(); };
    [[maybe_unused]] auto exceptionBase_object = [](Runtime& rt) { return rt.objectType(); };
    FOR_EACH_PY_EXCEPTION(PY_DEFINE_BASE_GETTER)
    FOR_EACH_PY_EXCEPTION(PY_INITIALIZE_EXCEPTION)
#undef PY_DEFINE_BASE_GETTER
#undef PY_INITIALIZE_EXCEPTION

    initializeExceptionGroupTypes(globalObject);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
