/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyNumbers.h"

// PyLong_*, PyFloat_*

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(Numbers)

PY_CAPI Handle PyLong_FromLong(long value)
{
    CAPI_NOISY();
    if (fitsSmallInt(static_cast<int64_t>(value))) [[likely]]
        return toC(smallInt(static_cast<int64_t>(value)));
    CAPI_ENTER(nullptr);
    return entry.newReference(makeBigInt(entry.globalObject, static_cast<int64_t>(value)));
}

// -1 and an exception: it is no int and has no __index__, or does not fit.
PY_CAPI long PyLong_AsLong(Handle object)
{
    CAPI_NOISY();
    JSValue value = toJS(object);
    if (value.isInt32()) [[likely]]
        return value.asInt32();
    CAPI_ENTER(-1);
    if (!value) {
        entry.raiseBadInternalCall();
        return -1;
    }
    static_assert(sizeof(long) == sizeof(int64_t));
    int64_t result = toInt64(entry.globalObject, value);
    return entry.failed() ? -1 : result;
}

PY_CAPI Handle PyFloat_FromDouble(double value)
{
    CAPI_ENTER(nullptr);
    return entry.newReference(makeFloat(entry.globalObject, value));
}

// -1.0 and an exception: it is no float and has neither __float__ nor __index__.
PY_CAPI double PyFloat_AsDouble(Handle object)
{
    CAPI_NOISY();
    JSValue value = toJS(object);
    if (value && isFloat(value)) [[likely]]
        return floatValue(value);
    CAPI_ENTER(-1.0);
    if (!value) {
        entry.raise(entry.runtime().TypeError(), "bad argument type for built-in operation"_s);
        return -1.0;
    }
    double result = toDouble(entry.globalObject, value);
    return entry.failed() ? -1.0 : result;
}

} // namespace CApi
} // namespace Python
} // namespace JSC
