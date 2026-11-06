/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "JSCInlines.h"
#include "PyBytes.h"
#include "PyTuple.h"

#if PYTHON_HAS_CAPI

// References, identity, the type of an object, the constants. None of these runs code of the program or raises but where it says so.

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(References)

PY_CAPI void _Py_IncRef(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    retain(object);
}

PY_CAPI void _Py_DecRef(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    release(object);
}

PY_CAPI void Py_IncRef(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    if (object)
        retain(object);
}

PY_CAPI void Py_DecRef(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    if (object)
        release(object);
}

PY_CAPI Handle Py_NewRef(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    retain(object);
    return object;
}

PY_CAPI Handle Py_XNewRef(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    if (object)
        retain(object);
    return object;
}

// What C owns, and two more: for whoever lent C the reference it asks with, and for whatever else refers to the object, which nobody has counted. So it is never 1, which
// would tell C that nobody else can see the object, and that it may change it in place. For what is not counted: what CPython says of an object that lives for ever.
static constexpr ssize_t countOfWhatLivesForEver = static_cast<ssize_t>(3) << 30;

PY_CAPI ssize_t Py_REFCNT(Handle object)
{
    CAPI_QUIET_AND_ONLY_READS(countOfWhatLivesForEver);
    if (JSCell* cell = countedCell(object))
        return static_cast<ssize_t>(current->counts.get(cell)) + 2;
    return countOfWhatLivesForEver;
}

PY_CAPI Handle Py_TYPE(Handle object)
{
    CAPI_QUIET_AND_ONLY_READS(nullptr);
    return toC(typeOf(object));
}

PY_CAPI int Py_IS_TYPE(Handle object, Handle type)
{
    CAPI_QUIET_AND_ONLY_READS(0);
    return JSValue(typeOf(object)) == toLookAt(type);
}

// One object may be spelled in two ways for C too: None and the bools are, by their symbols, which say what they stand for at all times, and as toC() spells them. (A type is only ever
// spelled by its symbol, a number in one way, and what the symbols of Ellipsis and NotImplemented stand for is there while the API is.)
static ALWAYS_INLINE JSValue plain(Handle handle)
{
    JSValue value = toLookAt(handle);
    return value.isNull() ? jsUndefined() : value;
}

PY_CAPI int Py_Is(Handle a, Handle b)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    if (a == b)
        return true;
    JSValue first = plain(a);
    return first && first == plain(b);
}

PY_CAPI int Py_IsNone(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return plain(object).isUndefined();
}

PY_CAPI int Py_IsTrue(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return plain(object).isTrue();
}

PY_CAPI int Py_IsFalse(Handle object)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return plain(object).isFalse();
}

// Those that are no cells. Py_None is such a call, and C compares with it wherever it is.
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
static Handle constantThatIsACell(unsigned id)
{
    CAPI_QUIET(nullptr);
    switch (id) {
    case 3:
        return toC(current->runtime.ellipsis());
    case 4:
        return toC(current->runtime.notImplemented());
    case 7:
        return toC(jsEmptyString(current->vm));
    case 8:
        return toC(current->runtime.emptyBytes());
    case 9:
        return toC(current->runtime.emptyTuple());
    default:
        break;
    }
    CAPI_ENTER_AFTER_ALL(nullptr);
    entry.raiseBadInternalCall();
    return nullptr;
}

PY_CAPI Handle Py_GetConstantBorrowed(unsigned id)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    if (JSValue value = constantThatIsNoCell(id))
        return toC(value);
    return constantThatIsACell(id);
}

PY_CAPI Handle Py_GetConstant(unsigned id)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    Handle result = Py_GetConstantBorrowed(id);
    if (result)
        retain(result);
    return result;
}

// Whether the interpreter that the API serves is there. It need not have been set up.
PY_CAPI int Py_IsInitialized()
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return !!s_runtimeItIsFor.load(std::memory_order_relaxed);
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
