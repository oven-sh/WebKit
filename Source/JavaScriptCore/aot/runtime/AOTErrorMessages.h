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
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC {

class Identifier;
class JSFunction;
class JSGlobalObject;
class JSObject;
class StackFrame;
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
    TakeOutOfRequireCache,
    StoreUnderNameOfModuleInRequireCache,
};

// The engine's own errors, worded by the engine.
// The message either ends in "(evaluating '..')" with all that is quoted, or takes the quote apart in a way of its own.
JS_EXPORT_PRIVATE String quotedSourceOfFailedExpression(VM&, bool messageEndsInQuote);
// A native constructor runs in the top frame, for a `new` expression of compiled code: the callee as the expression writes it.
StringView nameOfConstructorThatRunsOnTop(VM&);
// An operation runs in the top frame, for an `instanceof` of compiled code: the expression as it is written. Null for any other frame and where the site holds no text.
StringView instanceofExpressionOnTop(VM&);
StringView nameOfCalleeForwardedOnTop(VM&);
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
// An operation runs in the top frame, for the read of such an update: the read makes that test.
bool readOnTopTestsObjectOfUpdate(VM&);
// The function whose text holds the place where the frame on top stands, and the line of the place, counted from the line of the function's definition.
// A function that has no code of its own, because each call holds a copy, has no line.
struct PlaceInFunctionText {
    // The address that the place returns to. No two places share one, and an image stays for as long as the process.
    const void* address;
    FunctionID function;
    std::optional<unsigned> line;
    // In the file.
    ModuleID module;
    LineColumn position;
    // `const [[x]] = v`: a stack trace shows both lists at one place. The position is that of the `[` of the list which takes values at this place.
    bool isWhereListIsWritten { false };
};
std::optional<PlaceInFunctionText> placeOnTopInFunctionText(VM&);
// A function of the engine's own C++ runs: in a frame of its own above compiled code, or in an operation that stands for the call, whether or not its site names the built-in. The place is that
// of the call in the frame that a stack trace lists first, which may be of another image than the one that runs. Its image runs while the functor does.
void withPlaceOfCallOfNativeFunction(VM&, const ScopedLambda<void(const PlaceInFunctionText&)>&);
// Where the place on top says nothing: `f.call(receiver)` and `f.apply(receiver, list)` fail in the function of the library that forwards the call, and are written at the place that
// called it. Where images share realms, that place may be of another image, with nothing but hidden frames of the library above it. Its image runs while the functor does.
void withPlaceThatCalledLibraryOnTop(VM&, const ScopedLambda<void(const PlaceInFunctionText&)>&);
JSObject* createNotAFunctionError(JSGlobalObject*, JSValue callee, StringView name);
JSObject* createNotAFunctionError(JSGlobalObject*, JSValue callee, const Identifier& name);
JSObject* createNotAConstructorError(JSGlobalObject*, JSValue callee, StringView name);
// The engine enters a native function that is no constructor, and the function refuses in a frame of its own.
// `super(..)`
JSObject* createParentNotAConstructorError(JSGlobalObject*, JSValue parent);
void throwNotAConstructorErrorInFrameOf(JSGlobalObject*, JSFunction*, StringView name, bool isParent);
JSObject* createInstanceofRightHandSideNotAFunctionError(JSGlobalObject*, JSValue constructor, StringView expression);
JSObject* createHasInstanceNotAFunctionError(JSGlobalObject*, JSValue constructor, StringView expression);

JSObject* createBuiltinModulesUnavailableError(JSGlobalObject*);
JSObject* createNativeBindingsUnavailableError(JSGlobalObject*);
JSObject* createBuiltinWithoutPathError(JSGlobalObject*);
// An error whose sentence says that compiled code does not do what `bun run` does. With Options::aotCountsPath() it is counted where it is made, so also where a `catch` swallows it and the program takes another road.
JS_EXPORT_PRIVATE bool isRefusal(const String& message);
JS_EXPORT_PRIVATE void countRefusal(VM&, const String& message, const WTF::Vector<StackFrame>*);
JSObject* createBuiltinNotCompiledError(JSGlobalObject*, const String& builtin);
JSObject* createBuiltinFunctionNotCompiledError(JSGlobalObject*, const String& functionName);
JSObject* createBuiltinsNotCompiledError(JSGlobalObject*, std::span<const String> builtins);
JSObject* createExternalModuleNotLoadedError(JSGlobalObject*, const String& path);
// `code`: what Node.js gives the error of a load that finds nothing: MODULE_NOT_FOUND for require(), ERR_MODULE_NOT_FOUND for import(). Programs that try an optional package go by it.
JSObject* createLoadByNameRefusedError(JSGlobalObject*, StringView name, ASCIILiteral code, StringView asker = { });
JSObject* createExportClosedToOutsideError(JSGlobalObject*, StringView exportName, StringView module, StringView asker);
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
// The compiled code that is running, or that has called the built-in function which is, stands in a file of a test run that an executable holds too.
// Such a file meets what it meets there: the same refusal in the same words, and `false` from Reflect.
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
