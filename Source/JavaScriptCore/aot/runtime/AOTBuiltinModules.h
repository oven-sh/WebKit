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

class JSCell;
class JSFunction;
class JSGlobalObject;
class JSObject;
class JSValue;
class NativeExecutable;
class ScriptExecutable;
class VM;

namespace AOT {

struct State;

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
JS_EXPORT_PRIVATE bool hasBuiltinFunction(VM&, uint32_t id);
JS_EXPORT_PRIVATE JSFunction* builtinFunction(JSGlobalObject*, uint32_t id, JSObject* prototype = nullptr);
JS_EXPORT_PRIVATE NativeExecutable* builtinExecutable(JSGlobalObject*, uint32_t id);
std::optional<FunctionID> compiledBuiltinFunctionID(const JSCell*);

// The host can run a module or a function of its library from the text. That text requires the modules of the library, and an image without ImageFlag::AdmitsLibraryText holds only what seen code reaches of them.
JS_EXPORT_PRIVATE bool refusesLibraryText();
// A realm that the program creates holds nothing that was compiled: there the text of the library meets only itself.
JS_EXPORT_PRIVATE bool refusesLibraryText(JSGlobalObject*);
bool refusesToInterpret(JSGlobalObject*, ScriptExecutable&);
JS_EXPORT_PRIVATE bool refusesToRun(JSFunction*);
// The build found that nothing the program runs calls this function of the library, so the realm has only its text, and either the text is refused or all that runs beside the image are
// copies of its files, which call what the files call. An image with ImageFlag::AdmitsLibraryText would run the text, and the text calls what else the build has left out.
// The host asks this where it has something smaller to do in the function's place. Code that nobody has read may need the function: for that the text is there.
JS_EXPORT_PRIVATE bool isNeededByNothingThatRuns(JSFunction*);
void throwLibraryFunctionNotCompiled(JSGlobalObject*, ScriptExecutable&);
// `builtin`: its name as a program writes it.
JS_EXPORT_PRIVATE void throwBuiltinNotCompiled(JSGlobalObject*, const String& builtin);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
