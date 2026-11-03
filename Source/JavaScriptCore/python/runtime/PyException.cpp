/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyException.h"

#include "ErrorHandlingScope.h"
#include "ErrorInstance.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
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
    // A RecursionError is made where the stack is as good as used up: what is kept in reserve for handling errors is for this.
    VM& vm = getVM(globalObject);
    std::optional<ErrorHandlingScope> errorHandlingScope;
    if (type == runtime(globalObject).RecursionError() && vm.stackPointerAtVMEntry())
        errorHandlingScope.emplace(vm);
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

JSValue exceptionFromThrownValue(JSGlobalObject* globalObject, JSValue thrown)
{
    if (PyException::is(thrown))
        return thrown;

    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (auto* error = dynamicDowncast<ErrorInstance>(thrown)) {
        if (error->isStackOverflowError())
            return newException(globalObject, rt.RecursionError(), "maximum recursion depth exceeded"_s);
        if (error->isOutOfMemoryError())
            return newException(globalObject, rt.MemoryError(), std::span<const JSValue> { });
    }
    // An object of JS keeps its JSError. (Not one of Python's: the hidden fields of those belong to their types.)
    PyType* thrownType = rt.typeOf(thrown);
    bool remembers = thrownType == rt.jsObjectType() || thrownType == rt.jsFunctionType();
    if (remembers) {
        if (JSValue existing = hiddenField(globalObject, thrown, jsErrorHiddenField))
            return existing;
    }
    String message = thrown.toWTFString(globalObject);
    if (scope.exception()) {
        scope.clearException();
        message = "<exception>"_s;
    }
    PyException* exception = newException(globalObject, rt.JSError(), message);
    exception->setField(vm, ExceptionField::jsErrorValue, thrown);
    if (remembers)
        setHiddenField(globalObject, thrown, jsErrorHiddenField, exception);
    return exception;
}

PyException* exceptionOfThrownValueIfKnown(JSGlobalObject* globalObject, JSValue thrown)
{
    if (auto* exception = PyException::tryFrom(thrown))
        return exception;
    if (!thrown.isObject())
        return nullptr;
    Runtime& rt = runtime(globalObject);
    PyType* thrownType = rt.typeOf(thrown);
    if (thrownType != rt.jsObjectType() && thrownType != rt.jsFunctionType())
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
        raiseTypeError(globalObject, scope, "exceptions must derive from BaseException"_s);
        return;
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
            raiseTypeError(globalObject, scope, "exception causes must derive from BaseException"_s);
            return;
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
    if (!rejectKeywords(globalObject, object, args))
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
        return str(globalObject, args->at(0));
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
static bool initWithKeywords(JSGlobalObject* globalObject, JSValue object, const CallArgs& args, std::initializer_list<std::pair<ASCIILiteral, unsigned>> keywords)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = self(object);
    exception->setArgs(vm, PyTuple::create(globalObject, args.positional()));
    for (auto& keyword : keywords)
        exception->setField(vm, keyword.second, { });
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
            raiseTypeError(globalObject, scope, typeName(globalObject, object), "() got an unexpected keyword argument '"_s, StringView(*name), '\'');
            return false;
        }
    }
    return true;
}

static JSValue importErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    if (!initWithKeywords(globalObject, object, args, { { "name"_s, ExceptionField::importErrorName }, { "path"_s, ExceptionField::importErrorPath }, { "name_from"_s, ExceptionField::importErrorNameFrom } }))
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
    return makeStr(globalObject, builder);
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
    if (!initWithKeywords(globalObject, object, args, { { "name"_s, ExceptionField::nameErrorName } }))
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
    if (!initWithKeywords(globalObject, object, args, { { "name"_s, ExceptionField::attributeErrorName }, { "obj"_s, ExceptionField::attributeErrorObj } }))
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
        return repr(globalObject, args->at(0));
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

static void osErrorInitialize(JSGlobalObject* globalObject, PyException* exception, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    unsigned count = args.size();
    std::span<const JSValue> kept = args.positional();
    if (count >= 2 && count <= 5) {
        exception->setField(vm, ExceptionField::osErrorErrno, args[0]);
        exception->setField(vm, ExceptionField::osErrorStrerror, args[1]);
        if (count >= 3) {
            JSValue filename = args[2];
            if (exception->type()->isSubtypeOf(rt.BlockingIOError()) && isInt(filename) && count == 3)
                exception->setField(vm, ExceptionField::osErrorCharactersWritten, filename);
            else if (!isNone(filename)) {
                exception->setField(vm, ExceptionField::osErrorFilename, filename);
                if (count == 5 && !isNone(args[4]))
                    exception->setField(vm, ExceptionField::osErrorFilename2, args[4]);
                // The file names are not among the args, for compatibility.
                kept = kept.first(2);
            }
        }
    }
    exception->setArgs(vm, PyTuple::create(globalObject, kept));
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
    PyType* type = PyType::from(cls);
    bool usesInit = osErrorUsesInit(globalObject, type);
    if (!usesInit) {
        if (args.hasKeywords()) {
            auto scope = DECLARE_THROW_SCOPE(vm);
            return raiseTypeError(globalObject, scope, type->name(), "() takes no keyword arguments"_s);
        }
        if (type == rt.OSError() && args.size() >= 2 && args.size() <= 5 && isSmallInt(args[0])) {
            if (PyType* subclass = osErrorSubclassFor(rt, static_cast<int>(smallIntValue(args[0]))))
                type = subclass;
        }
    }
    PyException* exception = PyException::create(vm, rt.structure<PyException>(), type);
    if (usesInit)
        exception->setArgs(vm, rt.emptyTuple());
    else
        osErrorInitialize(globalObject, exception, args);
    return exception;
}

static JSValue osErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    if (!osErrorUsesInit(globalObject, self(object)->type()))
        return jsUndefined();
    if (!rejectKeywords(globalObject, object, args))
        return { };
    osErrorInitialize(globalObject, self(object), args);
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
    { "characters_written"_s, getCharactersWritten, setField<ExceptionField::osErrorCharactersWritten> },
};

// ---- SyntaxError ----

static JSValue syntaxErrorInit(JSGlobalObject* globalObject, JSValue object, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!baseExceptionInit(globalObject, object, args))
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
        if (length >= 6) {
            exception->setField(vm, ExceptionField::syntaxErrorEndLineno, info->at(4));
            exception->setField(vm, ExceptionField::syntaxErrorEndOffset, info->at(5));
        }
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
    if (hasFilename && hasLineNumber)
        return makeStr(vm, makeString(text, " ("_s, baseName, ", line "_s, intToString(globalObject, lineNumber), ')'));
    if (hasFilename)
        return makeStr(vm, makeString(text, " ("_s, baseName, ')'));
    return makeStr(vm, makeString(text, " (line "_s, intToString(globalObject, lineNumber), ')'));
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
};

// ---- UnicodeError ----

static JSValue unicodeErrorInitCommon(JSGlobalObject* globalObject, JSValue object, const CallArgs& args, bool hasEncoding, bool isDecoding)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!baseExceptionInit(globalObject, object, args))
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
        int64_t value = toSsize(globalObject, args[next++], nullptr);
        RETURN_IF_EXCEPTION(scope, { });
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
        if (!getBytesLike(globalObject, target, bytes))
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
        return runtime(globalObject).names.empty();

    JSString* reasonString = str(globalObject, noneIfEmpty(exception->field(ExceptionField::unicodeErrorReason)));
    RETURN_IF_EXCEPTION(scope, { });
    String reason = reasonString->value(globalObject);
    String encoding;
    if (kind != 2) {
        JSString* encodingString = str(globalObject, noneIfEmpty(exception->field(ExceptionField::unicodeErrorEncoding)));
        RETURN_IF_EXCEPTION(scope, { });
        encoding = encodingString->value(globalObject);
    }
    int64_t start = toSsize(globalObject, noneIfEmpty(exception->field(ExceptionField::unicodeErrorStart)), nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t end = toSsize(globalObject, noneIfEmpty(exception->field(ExceptionField::unicodeErrorEnd)), nullptr);
    RETURN_IF_EXCEPTION(scope, { });

    if (kind == 1) {
        std::span<const uint8_t> bytes;
        if (!getBytesLike(globalObject, target, bytes))
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
    int64_t position = toSsize(globalObject, value, nullptr);
    RETURN_IF_EXCEPTION(scope, void());
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

// ---- The types ----

// In CPython every type whose instances are laid out as those of BaseException has a __new__ of its own.
static constexpr NativeDef plainExceptionMethods[] = {
    PY_NEW(baseExceptionNew, "($type, *args, **kwargs)"),
};

void initializeExceptionTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();

    static constexpr OptionSet<TypeFlag> flags { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::HasWeakref };
    static constexpr OptionSet<TypeFlag> flagsWithFields { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::HasWeakref, TypeFlag::OwnLayout };

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
            def.getsets = jsErrorGetSets;
        } else if (base->solidBase() == rt.BaseException() && type != rt.BaseExceptionGroup() && type != rt.ExceptionGroup())
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

    rt.IncompleteInputError()->setNameString(vm, internString(vm, "_IncompleteInputError"_s));
    initializeExceptionGroupTypes(globalObject);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
