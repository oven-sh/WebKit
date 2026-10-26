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

// CalculatedDisplayName: what getCalculatedDisplayName() answers for the function object, which is the name in the lines `name@url:line:column` that the engine writes itself.
enum class NameForm : uint8_t { AsWritten, InFrames, CalculatedDisplayName };

// callee: the function object that runs in the frame, where the frame has recorded it. Nobody has seen one that lies in a frame, so it has no name but that of its function.
JS_EXPORT_PRIVATE String functionName(VM&, const FunctionDescription&, NameForm, JSObject* callee = nullptr, bool calleeLiesInFrame = false);
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
// A property `name` that was stored without a look for it: the first look puts the function's own name in its place. Frames show the stored one until then.
void showNameUntilLookedFor(VM&, JSObject* closure, const String& name);
// Behind whatever may have changed the own property `displayName` of the function object: a store, a definition, a deletion.
void didChangeDisplayName(VM&, JSObject* closure);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
