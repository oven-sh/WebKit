/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTRecordTypes.h"
#include "JSCJSValue.h"
#include <span>
#include <wtf/text/WTFString.h>

namespace JSC {

class Identifier;
class JSFunction;
class JSGlobalObject;
class JSObject;
class VM;

namespace AOT {

struct LayoutData;
struct LayoutSetData;
struct ProgramTables;
struct RuntimeType;

JS_EXPORT_PRIVATE String describe(JSValue);
String describe(const ProgramTables&, const RuntimeType&);
String describe(const ProgramTables&, const RuntimeType&, Typed::TypeSiteID);
String nameOf(const ProgramTables&, const LayoutData&);
String nameOf(const ProgramTables&, const LayoutSetData&);

enum class LoaderChange : uint8_t {
    ReplaceInRequireCache,
    ReplaceLoaderFunction,
};

// The engine's own errors, worded by the engine.
JS_EXPORT_PRIVATE String quotedSourceOfFailedExpression(VM&);
// A native constructor runs in the top frame, for a `new` expression of compiled code: the callee as the expression writes it.
StringView nameOfConstructorThatRunsOnTop(VM&);
// An operation runs in the top frame, for an `instanceof` of compiled code: the expression as it is written. Null for any other frame and where the site holds no text.
StringView instanceofExpressionOnTop(VM&);
StringView nameOfCalleeForwardedOnTop(VM&);
JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, StringView member);
JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, const Identifier& member);
JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, JSValue key);
// Without a member: the key is a value.
JSObject* createSuperBaseNotAnObjectError(JSGlobalObject*, JSValue base, const Identifier* member);
// `o[k]++`, `o[k] += v`, `o[k] ??= v`: bytecode tests `o` by an operation of its own, ahead of the conversion of `k` and of the read.
JSObject* createUpdateOfNullishError(JSGlobalObject*);
// An operation runs in the top frame, for the read of such an update: the read makes that test.
bool readOnTopTestsObjectOfUpdate(VM&);
JSObject* createNotAFunctionError(JSGlobalObject*, JSValue callee, StringView name);
JSObject* createNotAFunctionError(JSGlobalObject*, JSValue callee, const Identifier& name);
JSObject* createNotAConstructorError(JSGlobalObject*, JSValue callee, StringView name);
// The engine enters a native function that is no constructor, and the function refuses in a frame of its own.
void throwNotAConstructorErrorInFrameOf(JSGlobalObject*, JSFunction*, StringView name);
JSObject* createInstanceofRightHandSideNotAFunctionError(JSGlobalObject*, JSValue constructor, StringView expression);
JSObject* createHasInstanceNotAFunctionError(JSGlobalObject*, JSValue constructor, StringView expression);

JSObject* createBuiltinModulesUnavailableError(JSGlobalObject*);
JSObject* createNativeBindingsUnavailableError(JSGlobalObject*);
JSObject* createBuiltinWithoutPathError(JSGlobalObject*);
JSObject* createBuiltinNotCompiledError(JSGlobalObject*, const String& builtin);
JSObject* createBuiltinFunctionNotCompiledError(JSGlobalObject*, const String& functionName);
JSObject* createBuiltinsNotCompiledError(JSGlobalObject*, std::span<const String> builtins);
JSObject* createExternalModuleNotLoadedError(JSGlobalObject*, const String& path);
// `code`: what Node.js gives the error of a load that finds nothing: MODULE_NOT_FOUND for require(), ERR_MODULE_NOT_FOUND for import(). Programs that try an optional package go by it.
JSObject* createLoadByNameRefusedError(JSGlobalObject*, StringView name, ASCIILiteral code);
#if ENABLE(SHADOW_REALM)
JSObject* createModuleInShadowRealmError(JSGlobalObject*, StringView path);
#else
JSObject* createModuleInOtherRealmError(JSGlobalObject*, StringView path);
#endif
JSObject* createLoaderChangeRefusedError(JSGlobalObject*, LoaderChange, StringView changed);
JSObject* createModuleMissingAtBuildError(JSGlobalObject*, const String& specifier, const String& importer, ASCIILiteral code);
// `export { exportName } from "specifier"`, worded as the link of a module that is not compiled words it.
JSObject* createExportStatementFindsNothingError(JSGlobalObject*, const String& exportName, const String& specifier);
JSObject* createExportStatementFindsSeveralError(JSGlobalObject*, const String& exportName, const String& specifier);
JSObject* createSuperCallInDirectEvalError(JSGlobalObject*);
String functionNotCompiledMessage(const String& functionName);
String functionNotCompiledMessage(const String& path, unsigned line);
String functionNotCompiledMessage();
// `Array.prototype.push`, `Array.prototype[Symbol(Symbol.iterator)]`, and of the global object the name alone.
String memberOfLockedBuiltin(BuiltinID holder, UniquedStringImpl* key);
String assignmentToLockedBuiltinMessage(const String& property);
String deletionFromLockedBuiltinMessage(const String& property);
// call, apply, bind, Symbol.hasInstance: BuiltinTable::isFunctionOfEngine().
String assignmentToNameThatBuiltinFunctionInheritsMessage(UniquedStringImpl*);
String prototypeChangeOfLockedBuiltinMessage(const String& holder);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
