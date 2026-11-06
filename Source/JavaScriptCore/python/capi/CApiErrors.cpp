/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyCodecs.h"
#include "PyException.h"
#include "PyTuple.h"

#if PYTHON_HAS_CAPI

// PyErr_*: the error indicator, as C sees it.

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(Errors)

static bool isExceptionClass(Runtime& rt, JSValue value)
{
    PyType* type = PyType::tryFrom(value);
    return type && type->isSubtypeOf(rt.BaseException());
}

static bool isExceptionInstance(Runtime& rt, JSValue value)
{
    return rt.typeOf(value)->isSubtypeOf(rt.BaseException());
}

// The type of the exception that is set, borrowed: the exception holds it.
PY_CAPI Handle PyErr_Occurred()
{
    CAPI_QUIET(nullptr);
    Handle exception = errorIndicator();
    if (!exception) [[likely]]
        return nullptr;
    return toC(current->runtime.typeOf(toJS(exception)));
}

// The indicator is the thread's own, and still these three ask: what is in it is thrown by whoever called C, so nothing gets in but by a thread that runs C for the interpreter whose
// object it is, and nothing is left in it for another.
PY_CAPI void PyErr_Clear()
{
    CAPI_QUIET();
    setErrorIndicator(*current, nullptr);
}

PY_CAPI Handle PyErr_GetRaisedException()
{
    CAPI_QUIET(nullptr);
    return std::exchange(errorIndicator(), nullptr);
}

// Steals the reference.
PY_CAPI void PyErr_SetRaisedException(Handle exception)
{
    CAPI_QUIET();
    setErrorIndicator(*current, exception);
}

// raise type(*arguments), where the arguments are what `value` is taken for: nothing if there is none or it is None, its items if it is a tuple,
// else itself. An instance of the type is raised as it is.
static void setObject(Entry& entry, JSValue type, JSValue value)
{
    Runtime& rt = entry.runtime();
    // CPython follows the pointer.
    if (!type) {
        entry.raise(rt.SystemError(), "_PyErr_SetObject: the exception is NULL"_s);
        return;
    }
    if (!isExceptionClass(rt, type)) {
        String shown = reprForMessage(entry.globalObject, type);
        if (entry.failed())
            return;
        entry.raise(rt.SystemError(), makeString("_PyErr_SetObject: exception "_s, shown, " is not a BaseException subclass"_s));
        return;
    }
    {
        auto scope = DECLARE_THROW_SCOPE(entry.vm);
        PyType* exceptionType = PyType::from(type);
        if (value && isExceptionInstance(rt, value) && rt.typeOf(value)->isSubtypeOf(exceptionType))
            raiseException(entry.globalObject, scope, value);
        else {
            PyTuple* tuple = value ? PyTuple::tryFrom(value) : nullptr;
            std::span<const JSValue> arguments = !value || value.isUndefinedOrNull() ? std::span<const JSValue> { } : tuple ? tuple->span() : std::span<const JSValue> { &value, 1 };
            // The class may be the program's, and what comes of calling it may be anything.
            JSValue exception = call(entry.globalObject, exceptionType, CallArgs(arguments));
            if (scope.exception()) {
            } else if (isExceptionInstance(rt, exception))
                raiseException(entry.globalObject, scope, exception);
            else {
                String shown = reprForMessage(entry.globalObject, type);
                if (!scope.exception())
                    raiseTypeError(entry.globalObject, scope, "calling "_s, shown, " should have returned an instance of BaseException, not "_s, typeName(entry.globalObject, exception));
            }
        }
    }
    entry.failed();
}

PY_CAPI void PyErr_SetObject(Handle type, Handle value)
{
    CAPI_ENTER();
    setObject(entry, toJS(type), toJS(value));
}

PY_CAPI void PyErr_SetNone(Handle type)
{
    CAPI_ENTER();
    setObject(entry, toJS(type), { });
}

// The message is UTF-8. If it is not, that is the error.
PY_CAPI void PyErr_SetString(Handle type, const char* message)
{
    CAPI_ENTER();
    JSValue value = codecDecodeUTF8(entry.globalObject, byteCast<uint8_t>(unsafeSpan(message)), "strict"_s);
    if (!entry.failed())
        setObject(entry, toJS(type), value);
}

// Whether `except expected:` would catch `given`, which is an exception or a class. It raises nothing: what is no class matches itself.
static bool matches(Runtime& rt, JSValue given, JSValue expected)
{
    if (PyTuple* tuple = PyTuple::tryFrom(expected)) {
        for (JSValue item : tuple->span()) {
            if (matches(rt, given, item))
                return true;
        }
        return false;
    }
    if (isExceptionInstance(rt, given))
        given = rt.typeOf(given);
    if (isExceptionClass(rt, given) && isExceptionClass(rt, expected))
        return PyType::from(given)->isSubtypeOf(PyType::from(expected));
    return given == expected;
}

PY_CAPI int PyErr_GivenExceptionMatches(Handle given, Handle expected)
{
    CAPI_NOISY(0);
    return given && expected && matches(current->runtime, toJS(given), toJS(expected));
}

PY_CAPI int PyErr_ExceptionMatches(Handle expected)
{
    CAPI_NOISY(0);
    Handle given = errorIndicator();
    return given && expected && matches(current->runtime, toJS(given), toJS(expected));
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
