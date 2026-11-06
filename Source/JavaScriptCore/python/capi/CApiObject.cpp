/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyCodecs.h"
#include "PyStr.h"

// PyObject_*, PyUnicode_*

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(Object)

PY_CAPI Handle PyObject_Repr(Handle object)
{
    CAPI_ENTER(nullptr);
    if (!object)
        return entry.newReference(makeStr(entry.vm, "<NULL>"_s));
    return entry.newReference(reprObject(entry.globalObject, toJS(object)));
}

// The text is UTF-8.
PY_CAPI Handle PyUnicode_FromString(const char* text)
{
    CAPI_ENTER(nullptr);
    return entry.newReference(codecDecodeUTF8(entry.globalObject, byteCast<uint8_t>(unsafeSpan(text)), "strict"_s));
}

} // namespace CApi
} // namespace Python
} // namespace JSC
