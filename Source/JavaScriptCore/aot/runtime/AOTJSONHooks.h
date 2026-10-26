/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <span>
#include <wtf/text/StringView.h>

namespace JSC {

class JSGlobalObject;
class VM;

namespace AOT {

class StringBuilderStorage;

JSValue tryParseJSON(JSGlobalObject*, StringView);
JS_EXPORT_PRIVATE JSValue tryParseUTF8JSON(JSGlobalObject*, std::span<const uint8_t>);

// What JSON.stringify writes for a value that an object holds under a name, or an array at an index. False where it writes nothing for the value, and with an exception.
bool appendJSONTextOfProperty(JSGlobalObject*, StringBuilderStorage&, JSValue, JSValue property);

// For the file of counts: why a call of JSON.stringify was left to the general Stringifier.
void countReasonForSlowStringify(VM&, ASCIILiteral reason);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
