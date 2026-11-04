/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/AOTID.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <optional>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSCell;
class JSFunction;
class JSGlobalObject;
class JSObject;
class JSString;
class Structure;
class VM;

namespace AOT {

using Typed::FunctionID;

namespace CompiledCallee {

enum class FunctionKind : uint8_t { Sloppy, Strict, Arrow, Method, Class, Generator, Async, AsyncGenerator, Builtin };

JS_EXPORT_PRIVATE bool isCompiledCallee(const JSCell*);

JS_EXPORT_PRIVATE String name(JSObject* callee);
JS_EXPORT_PRIVATE unsigned length(JSObject* callee);
// Of a slim closure. It looks for neither property.
JS_EXPORT_PRIVATE bool canAssumeNameAndLengthAreOriginal(VM&, JSObject* callee);
JS_EXPORT_PRIVATE JSValue sourceText(JSGlobalObject*, JSObject* callee);
// The string that sourceText() returned for the callee's function, if it is still alive. Does not allocate.
JSString* sourceStringIfCreated(VM&, JSObject* callee);
JS_EXPORT_PRIVATE FunctionKind kind(JSObject* callee);
JS_EXPORT_PRIVATE bool isBoundFunction(JSObject* callee);

JS_EXPORT_PRIVATE void setPrototypeProperty(VM&, JSObject* callee, JSObject* prototype, unsigned attributes);
JS_EXPORT_PRIVATE void setNameProperty(JSGlobalObject*, JSObject* callee, JSString* name, unsigned attributes);
JS_EXPORT_PRIVATE void showNameUntilLookedFor(VM&, JSObject* callee, const String& name);

JS_EXPORT_PRIVATE String displayName(JSObject*);
// The text that the function object itself holds as the data property `displayName`, wherever it keeps it. Null without one.
JS_EXPORT_PRIVATE String ownDisplayName(JSObject*);
JS_EXPORT_PRIVATE bool reifyProperties(VM&, JSObject*);
JS_EXPORT_PRIVATE Structure* cachedBoundFunctionStructure(JSObject*);
JS_EXPORT_PRIVATE void setCachedBoundFunctionStructure(VM&, JSObject*, Structure*);

} // namespace CompiledCallee

enum class DeclaredAs : uint8_t { NotCompiled, Function, AsyncFunction, GeneratorFunction, AsyncGeneratorFunction, Class };

JS_EXPORT_PRIVATE DeclaredAs declaredAs(JSCell*);
// Of a compiled async function: whether its own body holds an `await`, a `for await` or an `await using`. What a function written inside it holds does not count.
JS_EXPORT_PRIVATE bool awaitsByItself(JSCell*);
JS_EXPORT_PRIVATE String closureDeclaredName(JSObject*);
JS_EXPORT_PRIVATE bool belongsToBuiltinModule(JSCell*);
JS_EXPORT_PRIVATE bool isCompiledClassObject(JSFunction*);
JS_EXPORT_PRIVATE bool isClosureOf(JSCell*, FunctionID);
std::optional<FunctionID> functionOf(const JSCell*);
std::optional<uint32_t> closureNumberOf(const JSCell*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
