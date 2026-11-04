/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
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

enum class LoaderChange : uint8_t {
    ReplaceInRequireCache,
    ReplaceLoaderFunction,
    TakeOutOfRequireCache,
    StoreUnderNameOfModuleInRequireCache,
};

// What the engine quotes in the message of an error that is made now: the text of the expression that fails in the compiled frame on top, as `bun build` prints it. Null: the image holds none.
JS_EXPORT_PRIVATE String quotedTextOfFailedExpression(VM&);

// The engine's own errors, in the engine's words. Where the image holds no text of the expression that failed, these spell it from the name that the site recorded:
// `undefined.member`, `super.member`, `name()`, `new name`. Without a name nothing is quoted.
JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, StringView member);
JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, const Identifier& member);
JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, JSValue key);
// `[a] = v` and `for (x of v)`, where `v` is undefined or null.
JSObject* createIterationOfNullishError(JSGlobalObject*, JSValue iterated);
// `k in base`
JSObject* createKeyTestOfNonObjectError(JSGlobalObject*, JSValue base);
// `delete base.k`
JSObject* createDeletionFromNullishError(JSGlobalObject*, JSValue base);
// `o.#x`, where `o` is an object without that name. `create` is the engine's function for the kind of name.
JSObject* createAbsentPrivateNameError(JSGlobalObject*, JSObject* (*create)(JSGlobalObject*));
// `o.#x`, `o.#x = v` where `o` is null or undefined.
JSObject* createPrivateNameOfNullishError(JSGlobalObject*, JSValue base);
// Without a member: the key is a value.
JSObject* createSuperBaseNotAnObjectError(JSGlobalObject*, JSValue base, const Identifier* member);
// `o[k]++`, `o[k] += v`, `o[k] ??= v`: bytecode tests `o` by an operation of its own, ahead of the conversion of `k` and of the read.
JSObject* createUpdateOfNullishError(JSGlobalObject*);
JSObject* createNotAFunctionError(JSGlobalObject*, JSValue callee, StringView name);
JSObject* createNotAFunctionError(JSGlobalObject*, JSValue callee, const Identifier& name);
JSObject* createNotAConstructorError(JSGlobalObject*, JSValue callee, StringView name);
// `super(..)`
JSObject* createParentNotAConstructorError(JSGlobalObject*, JSValue parent);
// The engine enters a native function that is no constructor, and the function refuses in a frame of its own.
void throwNotAConstructorErrorInFrameOf(JSGlobalObject*, JSFunction*, StringView name, bool isParent);

JSObject* createBuiltinModulesUnavailableError(JSGlobalObject*);
JSObject* createNativeBindingsUnavailableError(JSGlobalObject*);
JSObject* createBuiltinWithoutPathError(JSGlobalObject*);
JSObject* createBuiltinNotCompiledError(JSGlobalObject*, const String& builtin);
JSObject* createBuiltinFunctionNotCompiledError(JSGlobalObject*, const String& functionName);
JSObject* createBuiltinsNotCompiledError(JSGlobalObject*, std::span<const String> builtins);
JSObject* createExternalModuleNotLoadedError(JSGlobalObject*, const String& path);
// `code`: what Node.js gives the error of a load that finds nothing: MODULE_NOT_FOUND for require(), ERR_MODULE_NOT_FOUND for import(). Programs that try an optional package go by it.
JSObject* createLoadByNameRefusedError(JSGlobalObject*, StringView name, ASCIILiteral code);
JSObject* createFileOfImageRefusedError(JSGlobalObject*, StringView path, ASCIILiteral code);
JSObject* createModuleInOtherRealmError(JSGlobalObject*, StringView path);
JSObject* createLoaderChangeRefusedError(JSGlobalObject*, LoaderChange, StringView changed);
JSObject* createModuleMissingAtBuildError(JSGlobalObject*, const String& specifier, const String& importer, ASCIILiteral code);
// `export { exportName } from "specifier"`, worded as the link of a module that is not compiled words it.
JSObject* createExportStatementFindsNothingError(JSGlobalObject*, const String& exportName, const String& specifier);
JSObject* createExportStatementFindsSeveralError(JSGlobalObject*, const String& exportName, const String& specifier);
JSObject* createSuperCallInDirectEvalError(JSGlobalObject*);
String functionNotCompiledMessage(const String& functionName);
String exportNotCompiledMessage(const String& functionName, const String& module);
String functionNotCompiledMessage(const String& path, unsigned line);
String functionNotCompiledMessage();
// `Array.prototype.push`, `Array.prototype[Symbol(Symbol.iterator)]`, and of the global object the name alone.
String memberOfLockedBuiltin(BuiltinID holder, UniquedStringImpl* key);
String assignmentToLockedBuiltinMessage(const String& property);
String deletionFromLockedBuiltinMessage(const String& property);
// `property` may be assigned to where the build sees the assignment.
String assignmentThatBuildDidNotSeeMessage(const String& property);
String callOfBuiltinChangedBeforeFirstUseMessage(const String& property);
// A test run opens what the build sees a test replace. So there the words say what to write, or that nothing will do.
enum class LockedIn : bool { Executable, TestRun };
// In a test run: the write comes from a file that an executable would hold too (ModuleFlags::FindsBuiltinsReadOnly). The innermost compiled frame outside Bun's own code decides.
// Such a file gets what it gets in an executable: the same refusal message, and `false` from Reflect.
bool isWrittenByFileOfProduct(VM&);
String assignmentToLockedBuiltinMessage(VM&, BuiltinID holder, UniquedStringImpl* key, LockedIn);
String deletionFromLockedBuiltinMessage(VM&, BuiltinID holder, UniquedStringImpl* key, LockedIn);
// `property` is no entry of the table: hostDidFillObjectWithFoldedMembers(). No test run opens it.
String assignmentToFoldedMemberMessage(const String& property, LockedIn);
String deletionOfFoldedMemberMessage(const String& property, LockedIn);
// call, apply, bind, Symbol.hasInstance: BuiltinTable::isFunctionOfEngine().
String assignmentToNameThatBuiltinFunctionInheritsMessage(UniquedStringImpl*);
String prototypeChangeOfLockedBuiltinMessage(const String& holder, LockedIn = LockedIn::Executable);
String spyOnExportRefusedMessage(UniquedStringImpl* name, const String& path, LockedIn);
String mockOfModuleRefusedMessage(const String& path, LockedIn);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
