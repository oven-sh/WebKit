/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/AOTRecordTypes.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <optional>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class FunctionExecutable;
class JSCell;
class JSFunction;
class JSGlobalObject;
class JSObject;
class JSValue;
class NativeExecutable;
class ScriptExecutable;
class VM;

namespace AOT {

class Image;
struct State;
struct Instance;

class BuiltinModuleTable {
    WTF_MAKE_NONCOPYABLE(BuiltinModuleTable);
    WTF_MAKE_TZONE_ALLOCATED(BuiltinModuleTable);
public:
    explicit BuiltinModuleTable(State&);

    std::optional<ModuleID> find(uint32_t registryID) const;
    JSValue evaluate(ModuleID);

private:
    State& m_state;
};

JS_EXPORT_PRIVATE bool hasBuiltinModule(JSGlobalObject*, uint32_t registryID);
JS_EXPORT_PRIVATE JSValue evaluateBuiltinModule(JSGlobalObject*, uint32_t registryID);
JS_EXPORT_PRIVATE void willExposeBuiltinModuleToNativeCode(JSGlobalObject*, uint32_t registryID);
JS_EXPORT_PRIVATE void willExposeBuiltinExportToNativeCode(JSGlobalObject*, uint32_t registryID, ASCIILiteral exportName);
JS_EXPORT_PRIVATE JSValue evaluateEveryBuiltinModuleForTesting(JSGlobalObject*);
void fillWordsOfCompiledMethodsOfArray(Instance&);
JS_EXPORT_PRIVATE bool hasBuiltinFunction(VM&, uint32_t id);
JS_EXPORT_PRIVATE JSFunction* builtinFunction(JSGlobalObject*, uint32_t id, JSObject* prototype = nullptr);
JS_EXPORT_PRIVATE NativeExecutable* builtinExecutable(JSGlobalObject*, uint32_t id);
std::optional<FunctionID> compiledBuiltinFunctionID(const JSCell*);

JS_EXPORT_PRIVATE bool rejectsLibrarySource();
JS_EXPORT_PRIVATE bool rejectsLibrarySource(JSGlobalObject*);
bool isInterpretationDisallowed(JSGlobalObject*, ScriptExecutable&);
JS_EXPORT_PRIVATE void allowTestRunnerFunction(VM&, FunctionExecutable&);
JS_EXPORT_PRIVATE bool isFunctionDisallowed(JSFunction*);
JS_EXPORT_PRIVATE bool isUnreachableLibraryFunction(JSFunction*);
bool isClosedWorld(const Image&);
void throwLibraryFunctionNotCompiled(JSGlobalObject*, ScriptExecutable&);
JS_EXPORT_PRIVATE void throwBuiltinNotCompiled(JSGlobalObject*, const String& builtin);
JS_EXPORT_PRIVATE void noteGlobalObjectStoodInForReceiverOfEvents(JSGlobalObject*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
