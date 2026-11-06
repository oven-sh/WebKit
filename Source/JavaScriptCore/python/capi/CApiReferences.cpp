/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "JSCInlines.h"
#include "PyBytes.h"
#include "PyTuple.h"

// References, identity, the type of an object, the constants. None of these runs code of the program or raises but where it says so.

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(References)

PY_CAPI void _Py_IncRef(Handle object)
{
    CAPI_QUIET();
    retain(object);
}

PY_CAPI void _Py_DecRef(Handle object)
{
    CAPI_QUIET();
    release(object);
}

PY_CAPI void Py_IncRef(Handle object)
{
    CAPI_QUIET();
    if (object)
        retain(object);
}

PY_CAPI void Py_DecRef(Handle object)
{
    CAPI_QUIET();
    if (object)
        release(object);
}

PY_CAPI Handle Py_NewRef(Handle object)
{
    CAPI_QUIET();
    retain(object);
    return object;
}

PY_CAPI Handle Py_XNewRef(Handle object)
{
    CAPI_QUIET();
    if (object)
        retain(object);
    return object;
}

// What C owns, and two more: for whoever lent C the reference it asks with, and for whatever else refers to the object, which nobody has counted. So it is never 1, which
// would tell C that nobody else can see the object, and that it may change it in place. For what is not counted: what CPython says of an object that lives for ever.
PY_CAPI ssize_t Py_REFCNT(Handle object)
{
    CAPI_QUIET();
    if (JSCell* cell = countedCell(object); cell && mayEnter())
        return static_cast<ssize_t>(interpreter().counts.get(cell)) + 2;
    return static_cast<ssize_t>(3) << 30;
}

PY_CAPI Handle Py_TYPE(Handle object)
{
    CAPI_QUIET();
    return mayEnter() ? toC(interpreter().runtime.typeOf(toJS(object))) : nullptr;
}

PY_CAPI int Py_IS_TYPE(Handle object, Handle type)
{
    CAPI_QUIET();
    return mayEnter() && JSValue(interpreter().runtime.typeOf(toJS(object))) == toJS(type);
}

// One object may be spelled in two ways: see toC().
PY_CAPI int Py_Is(Handle a, Handle b)
{
    CAPI_QUIET();
    // Nothing is looked into: any thread may ask, at any time.
    auto plain = [](Handle handle) {
        JSValue value = toJS(handle);
        return value.isNull() ? jsUndefined() : value;
    };
    return plain(a) == plain(b);
}

PY_CAPI int Py_IsNone(Handle object)
{
    CAPI_QUIET();
    return toJS(object).isUndefinedOrNull();
}

PY_CAPI int Py_IsTrue(Handle object)
{
    CAPI_QUIET();
    return toJS(object).isTrue();
}

PY_CAPI int Py_IsFalse(Handle object)
{
    CAPI_QUIET();
    return toJS(object).isFalse();
}

// Those that are no cells. A library may ask for them as it is loaded (Py_None is such a call), before there is an interpreter to ask.
static JSValue constantThatIsNoCell(unsigned id)
{
    switch (id) {
    case 0:
        return jsUndefined();
    case 1:
        return jsBoolean(false);
    case 2:
        return jsBoolean(true);
    case 5:
        return jsNumber(0);
    case 6:
        return jsNumber(1);
    default:
        return { };
    }
}

// All of them live as long as the runtime.
static JSValue constant(unsigned id)
{
    Interpreter& current = interpreter();
    switch (id) {
    case 3:
        return current.runtime.ellipsis();
    case 4:
        return current.runtime.notImplemented();
    case 7:
        return jsEmptyString(current.vm);
    case 8:
        return current.runtime.emptyBytes();
    case 9:
        return current.runtime.emptyTuple();
    default:
        return { };
    }
}

PY_CAPI Handle Py_GetConstantBorrowed(unsigned id)
{
    CAPI_QUIET();
    if (JSValue value = constantThatIsNoCell(id))
        return toC(value);
    if (!mayEnter())
        return nullptr;
    if (JSValue value = constant(id)) [[likely]]
        return toC(value);
    CAPI_ENTER(nullptr);
    entry.raiseBadInternalCall();
    return nullptr;
}

PY_CAPI Handle Py_GetConstant(unsigned id)
{
    CAPI_QUIET();
    Handle result = Py_GetConstantBorrowed(id);
    if (result)
        retain(result);
    return result;
}

PY_CAPI int Py_IsInitialized()
{
    CAPI_QUIET();
    // Any thread may ask, with the GIL or without.
    if (s_interpreter.load(std::memory_order_relaxed))
        return true;
    return mayEnter();
}

} // namespace CApi
} // namespace Python
} // namespace JSC
