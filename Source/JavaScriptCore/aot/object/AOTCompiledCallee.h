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
JS_EXPORT_PRIVATE JSValue sourceText(JSGlobalObject*, JSObject* callee);
JS_EXPORT_PRIVATE FunctionKind kind(JSObject* callee);
JS_EXPORT_PRIVATE bool isBoundFunction(JSObject* callee);

JS_EXPORT_PRIVATE void setPrototypeProperty(VM&, JSObject* callee, JSObject* prototype, unsigned attributes);
JS_EXPORT_PRIVATE void setNameProperty(JSGlobalObject*, JSObject* callee, JSString* name, unsigned attributes);

JS_EXPORT_PRIVATE String displayName(JSObject*);
JS_EXPORT_PRIVATE bool reifyProperties(VM&, JSObject*);
JS_EXPORT_PRIVATE Structure* cachedBoundFunctionStructure(JSObject*);
JS_EXPORT_PRIVATE void setCachedBoundFunctionStructure(VM&, JSObject*, Structure*);

} // namespace CompiledCallee

enum class DeclaredAs : uint8_t { NotCompiled, Function, AsyncFunction, GeneratorFunction, AsyncGeneratorFunction, Class };

JS_EXPORT_PRIVATE DeclaredAs declaredAs(JSCell*);
JS_EXPORT_PRIVATE String closureDeclaredName(JSObject*);
JS_EXPORT_PRIVATE bool belongsToBuiltinModule(JSCell*);
JS_EXPORT_PRIVATE bool isCompiledClassObject(JSFunction*);
JS_EXPORT_PRIVATE bool isClosureOf(JSCell*, FunctionID);
std::optional<FunctionID> functionOf(const JSCell*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
