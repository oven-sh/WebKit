/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include "JSExportMacros.h"

namespace JSC {
class JSGlobalObject;
}

// The members of %CompilerHelpers% that are native code of the engine.
extern "C" {

JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__ownEntriesFlat(JSC::JSGlobalObject*, JSC::EncodedJSValue object);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__describeUnlessOrdinary(JSC::JSGlobalObject*, JSC::EncodedJSValue source);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__definePropertiesFrom(JSC::JSGlobalObject*, JSC::EncodedJSValue target, JSC::EncodedJSValue described, JSC::EncodedJSValue source);

JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__scanPlainQuery(JSC::JSGlobalObject*, JSC::EncodedJSValue text);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__findInPlainQuery(JSC::JSGlobalObject*, JSC::EncodedJSValue text, JSC::EncodedJSValue name);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__plainQueryEntries(JSC::JSGlobalObject*, JSC::EncodedJSValue text, JSC::EncodedJSValue query);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__plainQueryKeys(JSC::JSGlobalObject*, JSC::EncodedJSValue text, JSC::EncodedJSValue query);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__plainQueryValues(JSC::JSGlobalObject*, JSC::EncodedJSValue text, JSC::EncodedJSValue query);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__scanPlainURL(JSC::EncodedJSValue text);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__scanPlainURLWithPlainBase(JSC::EncodedJSValue text);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__scanPlainURLWithBase(JSC::EncodedJSValue text, JSC::EncodedJSValue base);
JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__findPlainQueryValue(JSC::EncodedJSValue text, JSC::EncodedJSValue offsets, JSC::EncodedJSValue name);

} // extern "C"

#endif // ENABLE(AOT)
