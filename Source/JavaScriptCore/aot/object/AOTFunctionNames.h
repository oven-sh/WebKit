/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "JSExportMacros.h"
#include <wtf/text/WTFString.h>

namespace JSC {

class Identifier;
class JSGlobalObject;
class JSObject;
class JSString;
class VM;

namespace AOT {

class FunctionDescription;
struct State;

enum class NameForm : uint8_t { AsWritten, InFrames };

JS_EXPORT_PRIVATE String functionName(const FunctionDescription&, NameForm);
JSString* functionSourceText(VM&, FunctionID);

String computedNameInFrames(VM&, const FunctionDescription&);
String writtenFunctionName(FunctionID);
String initialNameProperty(FunctionID);
JSString* initialNamePropertyString(VM&, State&, FunctionID);

void setComputedFunctionName(VM&, JSObject* closure, const Identifier& key, MethodKind);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
