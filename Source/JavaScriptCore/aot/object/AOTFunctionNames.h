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

struct State;

enum class NameForm : uint8_t { AsWritten, InFrames };

// callee: the function object that runs in the frame, where the frame has recorded it.
JS_EXPORT_PRIVATE String functionName(VM&, FunctionID, NameForm, JSObject* callee = nullptr);
JSString* functionSourceText(JSGlobalObject*, FunctionID);

String writtenFunctionName(FunctionID);
String initialNameProperty(FunctionID);
JSString* initialNamePropertyString(VM&, State&, FunctionID);

void setComputedFunctionName(VM&, JSObject* closure, const Identifier& key, MethodKind);

void recordCallee(VM&, JSObject* closure);

void didDefineNameProperty(VM&, JSObject* closure, const String& name);
void didDeleteNameProperty(VM&, JSObject* closure);
// The engine makes `name` a property of the function object when somebody first looks for it there: a read, a test, a list of the own keys, a definition.
void didLookForNameProperty(VM&, JSObject* closure);
// Behind whatever may have changed the own property `displayName` of the function object: a store, a definition, a deletion.
void didChangeDisplayName(VM&, JSObject* closure);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
