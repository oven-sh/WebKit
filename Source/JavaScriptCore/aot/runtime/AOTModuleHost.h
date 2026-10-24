/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <JavaScriptCore/AOTRecordTypes.h>
#include <JavaScriptCore/ArgList.h>
#include <JavaScriptCore/Identifier.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <optional>
#include <wtf/IterationStatus.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/StringView.h>

namespace JSC {

class AbstractModuleRecord;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSPromise;
class JSString;

namespace AOT {

struct ModuleObjects {
    JSObject* module { nullptr };
    JSValue require;
};

enum class ModuleObjectTrait : uint8_t {
    // `module.id` is ".", and `module.parent` is null.
    IsMain = 1 << 0,
    // `module.exports` is a variable of the program: moduleExports() reads it and setModuleExports() stores it. No code sees the module object of any other module.
    HasExportsVariable = 1 << 1,
};

enum class ResolvedSpecifierForm : uint8_t { Path, PathForRequire, URL };
enum class LoadByName : uint8_t { Require, Import };
enum class RequireOverride : uint8_t {
    ResolveFilename = 1 << 0,
    RequireFunction = 1 << 1,
    Extensions = 1 << 2,
};

// A host without the thing that a member is for leaves the member null.
struct ModuleHost {
    void (*createModuleObject)(JSGlobalObject*, JSString* path, ModuleID, OptionSet<ModuleObjectTrait>, ModuleObjects&);
    // For a file that is no CommonJS file, where only `require` has a name.
    void (*createRequirer)(JSGlobalObject*, JSObject* importMeta, ModuleObjects&);
    JSValue (*requireSpecifier)(JSGlobalObject*, JSObject* requirer, JSValue specifier);
    JSValue (*resolveSpecifier)(JSGlobalObject*, JSObject* requirerOrImportMeta, JSValue specifier, ResolvedSpecifierForm);
    // An empty value stands for an export that is read from the returned object, under its name, when something first binds it. Null: every value is there.
    JSObject* (*captureSyntheticExports)(JSGlobalObject*, SyntheticExportRule, JSValue value, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    // For a built-in module that has no JavaScript source.
    JSValue (*requireNativeModule)(JSGlobalObject*, JSString* specifier);
    // The host lists the exports itself: they need not be the properties of what require() returns.
    JSObject* (*captureNativeModuleExports)(JSGlobalObject*, uint32_t registryID, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    JSObject* (*requireCache)(JSGlobalObject*);
    JSObject* (*requireExtensions)(JSGlobalObject*);
    JSObject* (*createAsyncModuleRequiredError)(JSGlobalObject*, const String& path);
    JSObject* (*createEmptyJSONBodyError)(JSGlobalObject*);
    // What loading the file as code throws before any of it runs. Empty where the host finds nothing wrong with the file. Null in a host that reads no code at run time.
    JSValue (*createLoadError)(JSGlobalObject*, const String& path);
};

const ModuleHost& moduleHostOf(JSGlobalObject*);

JS_EXPORT_PRIVATE std::optional<ModuleID> findModule(StringView publicPath);
JS_EXPORT_PRIVATE String modulePath(ModuleID);
JS_EXPORT_PRIVATE bool entryPointIsCommonJS(JSGlobalObject*);
JS_EXPORT_PRIVATE void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>&);
// Code: what the host would have to interpret or link at run time. Data files load in every executable.
JS_EXPORT_PRIVATE bool refusesCodeOutsideImage(JSGlobalObject*);
// A file whose loader answers an object of the host, a database or a page.
JS_EXPORT_PRIVATE bool refusesHostObjectsOutsideImage(JSGlobalObject*);

JS_EXPORT_PRIVATE JSValue moduleExports(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void setModuleExports(JSGlobalObject*, ModuleID, JSValue);
JS_EXPORT_PRIVATE JSObject* mainModuleObject(JSGlobalObject*);
// Of the file whose require() came to the module first. Null where the program recorded none.
JS_EXPORT_PRIVATE JSObject* parentModuleObject(JSGlobalObject*, ModuleID);
// Calls the functor with the module object of each CommonJS file that the module has required since the last call, in the order of the first require() of each.
JS_EXPORT_PRIVATE bool forEachNewChild(JSGlobalObject*, ModuleID, const ScopedLambda<void(JSObject*)>&);
JS_EXPORT_PRIVATE bool recordRequire(JSGlobalObject*, ModuleID target, ModuleID referrer);
// The host calls this with every name that a function object of the program is given to load: require, module.require, Module._load, process.getBuiltinModule.
JS_EXPORT_PRIVATE void willLoadByName(JSGlobalObject*, JSValue specifier, LoadByName = LoadByName::Require);
// The host calls this with the file of the image that such a name has resolved to, before it looks into a cache of its own. It throws for a file that is no entry point.
JS_EXPORT_PRIVATE void willAnswerWithModule(JSGlobalObject*, ModuleID, LoadByName = LoadByName::Require);

JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecord(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isCompiledModuleRecord(AbstractModuleRecord*);
JS_EXPORT_PRIVATE JSPromise* importModule(JSGlobalObject*, ModuleID, std::optional<int64_t> referrerAsyncOrder);
JS_EXPORT_PRIVATE void evaluateModuleSynchronously(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* moduleNamespace(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSValue requireModule(JSGlobalObject*, ModuleID);

// A module of the image runs at most once and is in no module cache, and the code calls the module that each constant name stood for in the build.
JS_EXPORT_PRIVATE void willSetRequireCacheEntry(JSGlobalObject*, ModuleID, JSValue entry);
JS_EXPORT_PRIVATE void willChangeRequireOverride(JSGlobalObject*, RequireOverride, bool isOverridden);
JS_EXPORT_PRIVATE bool hasFileWithExtension(StringView);

JS_EXPORT_PRIVATE bool isModuleNamespace(JSCell*);
JS_EXPORT_PRIVATE bool hasESModuleMarker(JSCell* moduleNamespace);
JS_EXPORT_PRIVATE void setESModuleMarker(JSCell* moduleNamespace, bool);

JS_EXPORT_PRIVATE void setVerbose(bool);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
