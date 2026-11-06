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
    CAPI_QUIET();
    Handle exception = errorIndicator();
    if (!exception) [[likely]]
        return nullptr;
    return mayEnter() ? toC(interpreter().runtime.typeOf(toJS(exception))) : nullptr;
}

PY_CAPI void PyErr_Clear()
{
    CAPI_QUIET();
    setErrorIndicator(nullptr);
}

PY_CAPI Handle PyErr_GetRaisedException()
{
    CAPI_QUIET();
    return std::exchange(errorIndicator(), nullptr);
}

// Steals the reference.
PY_CAPI void PyErr_SetRaisedException(Handle exception)
{
    CAPI_QUIET();
    setErrorIndicator(exception);
}

// raise type(*arguments), where the arguments are what `value` is taken for: nothing if there is none or it is None, its items if it is a tuple,
// else itself. An instance of the type is raised as it is.
PY_CAPI void PyErr_SetObject(Handle typeHandle, Handle valueHandle)
{
    CAPI_ENTER();
    Runtime& rt = entry.runtime();
    JSValue type = toJS(typeHandle);
    JSValue value = toJS(valueHandle);
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
        else if (!value || value.isUndefinedOrNull())
            raiseWithArgs(entry.globalObject, scope, exceptionType, { });
        else if (PyTuple* tuple = PyTuple::tryFrom(value)) {
            JSValue exception = call(entry.globalObject, exceptionType, CallArgs(tuple->span().data(), tuple->length()));
            if (!scope.exception())
                raiseException(entry.globalObject, scope, exception);
        } else
            raiseWithArgs(entry.globalObject, scope, exceptionType, { value });
    }
    entry.failed();
}

PY_CAPI void PyErr_SetNone(Handle type)
{
    CAPI_NOISY();
    PyErr_SetObject(type, nullptr);
}

// The message is UTF-8. If it is not, that is the error.
PY_CAPI void PyErr_SetString(Handle type, const char* message)
{
    CAPI_NOISY();
    Handle value;
    {
        CAPI_ENTER();
        value = entry.newReference(codecDecodeUTF8(entry.globalObject, byteCast<uint8_t>(unsafeSpan(message)), "strict"_s));
        if (!value)
            return;
    }
    PyErr_SetObject(type, value);
    release(value);
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
    if (!given || !expected)
        return 0;
    return matches(interpreter().runtime, toJS(given), toJS(expected));
}

PY_CAPI int PyErr_ExceptionMatches(Handle expected)
{
    CAPI_NOISY(0);
    return PyErr_GivenExceptionMatches(errorIndicator(), expected);
}

} // namespace CApi
} // namespace Python
} // namespace JSC
