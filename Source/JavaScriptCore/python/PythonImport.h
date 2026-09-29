/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#pragma once

#include "JSCJSValue.h"
#include <wtf/text/WTFString.h>

// Python/import.c of CPython, and what Python/ceval.c does for the statement. Finding a module and loading it is importlib's, which is written in Python and is CPython's own.

namespace JSC {

class FunctionExecutable;
class JSGlobalObject;
class JSObject;

namespace Python {

struct FrozenModule;
struct ImportState;

ImportState& importState(JSGlobalObject*);

// ---- sys.modules

// import_get_module(): what sys.modules has. Empty, with nothing raised, if it has nothing by that name.
JSValue moduleIfImported(JSGlobalObject*, JSValue name);
// PyImport_GetModule(): the same, once it has been run to its end.
JSValue getImportedModule(JSGlobalObject*, JSValue name);
// PyImport_AddModuleRef(): the module that sys.modules has, or a new and empty one that is put there. Null if it raised.
JSObject* addModule(JSGlobalObject*, JSValue name);

// ---- import

// PyImport_ImportModuleLevelObject(), which is what builtins.__import__() is. An empty value for the globals, the locals or the list is NULL.
JSValue importModuleLevel(JSGlobalObject*, JSValue name, JSValue globals, JSValue locals, JSValue fromList, int level);
// PyImport_ImportModule(): as the statement would, by whatever __import__ is where this is called from. It is the module that was named that is returned, and not the package that it is in.
JS_EXPORT_PRIVATE JSValue importModule(JSGlobalObject*, const String& name);
// PyImport_ImportModuleAttrString()
JSValue importModuleAttribute(JSGlobalObject*, const String& module, ASCIILiteral attribute);
// PyImport_GetImporter()
JSValue pathImporterFor(JSGlobalObject*, JSValue path);

// _PyEval_ImportName(), _PyEval_ImportFrom() and import_all_from()
JSValue importName(JSGlobalObject*, JSObject* builtins, JSValue globals, JSValue locals, JSValue name, JSValue fromList, JSValue level);
JSValue importFrom(JSGlobalObject*, JSValue module, JSValue name);
void importAllFrom(JSGlobalObject*, JSValue locals, JSValue module);

// ---- What is known of a module from its __spec__: Objects/moduleobject.c

// Each is nothing if it raised.
std::optional<bool> isSpecInitializing(JSGlobalObject*, JSValue spec); // _PyModuleSpec_IsInitializing()
std::optional<bool> isUninitializedSubmodule(JSGlobalObject*, JSValue spec, JSValue name); // _PyModuleSpec_IsUninitializedSubmodule()
// _PyModuleSpec_GetFileOrigin(): the file that it is from, a str. Empty if it is from none, or if it raised.
JSValue fileOriginOfSpec(JSGlobalObject*, JSValue spec);
// _PyModule_IsPossiblyShadowing(): whether a module from that file could be in the way of one of the same name that is further along sys.path.
bool isPossiblyShadowing(JSGlobalObject*, JSValue origin);
// Whether it is, and what it is in the way of is part of the standard library. Nothing if it raised.
std::optional<bool> isShadowingStandardLibrary(JSGlobalObject*, JSValue moduleName);

// ---- The modules that are written in C++, and those that are written in Python and come with the engine or its host

int isBuiltinModule(JSGlobalObject*, const String& name); // is_builtin(): -1 if it is one that cannot be made again.
JSValue createBuiltinModule(JSGlobalObject*, JSValue name); // create_builtin(): None if there is none of that name. Empty if it raised.
JSValue builtinModuleNames(JSGlobalObject*); // sys.builtin_module_names
// PyImport_ImportFrozenModuleObject(): false if there is none of that name. Nothing if it raised.
std::optional<bool> importFrozenModule(JSGlobalObject*, JSValue name);

enum class FrozenStatus : uint8_t {
    Okay,
    BadName, // What was given is no name for a module.
    NotFound,
    Disabled, // -X frozen_modules=off, and it can be done without.
    Excluded, // It is there so as to say that it is not to be imported.
    Invalid, // There is nothing in it to run.
};

struct FrozenInfo {
    const FrozenModule* module { nullptr };
    String originalName;
};

FrozenStatus findFrozen(JSGlobalObject*, JSValue name, FrozenInfo&); // find_frozen()
void raiseFrozenError(JSGlobalObject*, FrozenStatus, JSValue name); // set_frozen_error()
FunctionExecutable* compileFrozen(JSGlobalObject*, const FrozenInfo&); // Null if it raised.
JSValue frozenModuleNames(JSGlobalObject*); // list_frozen_module_names()

// What a file of compiled code begins with, so that what was written by something else is not taken for it: two bytes, and then "\r\n" so that a file that has been through something that changes the ends of
// lines is found out. It is "JS", and is to be something else whenever what marshal writes for code is.
static constexpr int32_t pycMagicNumberToken = 'J' | ('S' << 8) | ('\r' << 16) | ('\n' << 24);

// ---- When a realm is made, and before a program is run

// _PyImport_InitCore(): after this, what is written in C++ and what is frozen can be imported. It cannot fail.
void initializeImport(JSGlobalObject*);
// _PyImport_InitExternal(): after this, so can what is in files.
void initializeExternalImport(JSGlobalObject*);
// All that is done before a program is run and can go wrong, if it has not been done: init_interp_main() of CPython's Python/pylifecycle.c. Whatever runs Python from outside calls it first.
void startPython(JSGlobalObject*);

// remove_importlib_frames(): takes importlib's own frames out of the traceback of the exception that has been raised.
void removeImportlibFrames(JSGlobalObject*);

} } // namespace JSC::Python
