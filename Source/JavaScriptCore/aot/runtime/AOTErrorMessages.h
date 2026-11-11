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

enum class LoaderChange : uint8_t {
    ReplaceInRequireCache,
    ReplaceLoaderFunction,
    TakeOutOfRequireCache,
    StoreUnderNameOfModuleInRequireCache,
    EvaluateModuleAgain,
    PassSourceToCompile,
};

JS_EXPORT_PRIVATE String failedExpressionSource(VM&);
JS_EXPORT_PRIVATE String primitiveKindForErrorMessage(JSValue);

JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, StringView member);
JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, const Identifier& member);
JSObject* createNotAnObjectError(JSGlobalObject*, JSValue base, JSValue key);
JSObject* createIterationOfNullishError(JSGlobalObject*, JSValue iterated);
JSObject* createKeyTestOfNonObjectError(JSGlobalObject*, JSValue base);
JSObject* createDeletionFromNullishError(JSGlobalObject*, JSValue base);
JSObject* createAbsentPrivateNameError(JSGlobalObject*, JSObject* (*create)(JSGlobalObject*));
JSObject* createPrivateNameOfNullishError(JSGlobalObject*, JSValue base);
JSObject* createSuperBaseNotAnObjectError(JSGlobalObject*, JSValue base, const Identifier* member);
JSObject* createUpdateOfNullishError(JSGlobalObject*);
JSObject* createNotAFunctionError(JSGlobalObject*, JSValue callee, StringView name);
JSObject* createNotAFunctionError(JSGlobalObject*, JSValue callee, const Identifier& name);
JSObject* createNotAConstructorError(JSGlobalObject*, JSValue callee, StringView name);
JSObject* createParentNotAConstructorError(JSGlobalObject*, JSValue parent);
void throwNotAConstructorErrorInFrameOf(JSGlobalObject*, JSFunction*, StringView name, bool isParent);

JSObject* createBuiltinModulesUnavailableError(JSGlobalObject*);
JSObject* createNativeBindingsUnavailableError(JSGlobalObject*);
JSObject* createBuiltinWithoutPathError(JSGlobalObject*);
JSObject* createBuiltinNotCompiledError(JSGlobalObject*, const String& builtin);
JSObject* createBuiltinBehindTargetOfEventNotCompiledError(JSGlobalObject*, const String& builtin);
JSObject* createBuiltinFunctionNotCompiledError(JSGlobalObject*, const String& functionName);
JSObject* createBuiltinsNotCompiledError(JSGlobalObject*, std::span<const String> builtins);
JSObject* createExternalModuleNotLoadedError(JSGlobalObject*, const String& path);
JSObject* createLoadByNameRefusedError(JSGlobalObject*, StringView name, ASCIILiteral code);
JSObject* createCompiledModuleAccessError(JSGlobalObject*, StringView path, ASCIILiteral code);
JSObject* createFormNotHeldError(JSGlobalObject*, StringView path, ASCIILiteral type);
JSObject* createExportNotHandedOutError(JSGlobalObject*, StringView path, StringView name);
JSObject* createModuleInOtherRealmError(JSGlobalObject*, StringView path);
JSObject* createLoaderChangeError(JSGlobalObject*, LoaderChange, StringView changed);
JSObject* createEntryOfRequireCacheRefusedError(JSGlobalObject*, StringView path);
JSObject* createComparisonOfPrivateFieldRefusedError(JSGlobalObject*, const String& field);
JSObject* createModuleMissingAtBuildError(JSGlobalObject*, const String& specifier, const String& importer, ASCIILiteral code);
JSObject* createExportStatementFindsNothingError(JSGlobalObject*, const String& exportName, const String& specifier);
JSObject* createExportStatementFindsSeveralError(JSGlobalObject*, const String& exportName, const String& specifier);
JSObject* createSuperCallInDirectEvalError(JSGlobalObject*);
String functionNotCompiledMessage(const String& functionName);
String exportNotCompiledMessage(const String& functionName, const String& module);
String functionNotCompiledMessage(const String& path, unsigned line);
String functionNotCompiledMessage();
String memberOfLockedBuiltin(BuiltinID holder, UniquedStringImpl* key);
String assignmentToLockedBuiltinMessage(const String& property);
String changeThatCompiledCodeRulesOutMessage(ObjectMutation);
String changeThatCompiledCodeRulesOutMessage(ASCIILiteral change);
String deletionFromLockedBuiltinMessage(const String& property);
String assignmentThatBuildDidNotSeeMessage(const String& property);
String moduleObjectRefusedMessage(StringView path);
String callOfBuiltinChangedBeforeFirstUseMessage(const String& property);
enum class LockedIn : bool { Executable, TestRun };
bool isWrittenByFileOfProduct(VM&);
String assignmentToLockedBuiltinMessage(VM&, BuiltinID holder, UniquedStringImpl* key, LockedIn);
String deletionFromLockedBuiltinMessage(VM&, BuiltinID holder, UniquedStringImpl* key, LockedIn);
String assignmentToFoldedMemberMessage(const String& property, LockedIn);
String deletionOfFoldedMemberMessage(const String& property, LockedIn);
String assignmentToNameThatBuiltinFunctionInheritsMessage(UniquedStringImpl*);
String prototypeChangeOfLockedBuiltinMessage(const String& holder, LockedIn = LockedIn::Executable);
String makeSpyOnExportErrorMessage(UniquedStringImpl* name, const String& path, LockedIn, const String& uncompiledCaller);
String makeMockModuleErrorMessage(const String& path, LockedIn, const String& uncompiledCaller);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
