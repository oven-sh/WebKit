/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/AOTRecordTypes.h>
#include <JavaScriptCore/ArgList.h>
#include <JavaScriptCore/Identifier.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <optional>
#include <wtf/IterationStatus.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace JSC {

class AbstractModuleRecord;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSPromise;
class JSString;

namespace AOT {

struct CommonJSModuleObjects {
    JSObject* module { nullptr };
    JSValue exports;
    JSValue require;
    JSValue filename;
    JSValue dirname;
};

enum class ChildLink : uint8_t { Recorded, ChildrenAreObservable };
enum class ModuleAdoption : uint8_t { Adopted, Rejected };
enum class ResolvedSpecifierForm : uint8_t { Path, URL };
enum class RequireOverride : uint8_t {
    ResolveFilename = 1 << 0,
    RequireFunction = 1 << 1,
    Extensions = 1 << 2,
};

struct ModuleHost {
    void (*createCommonJSModule)(JSGlobalObject*, JSString* path, JSObject* parent, CommonJSModuleObjects&);
    // Compiled code takes the objects for the ones the host creates, so the host rejects a module object of another class and one whose exports the program replaced.
    ModuleAdoption (*adoptCommonJSModule)(JSGlobalObject*, JSString* path, JSObject* module, CommonJSModuleObjects&);
    void (*createRequirer)(JSGlobalObject*, JSObject* importMeta, CommonJSModuleObjects&);
    ChildLink (*addChild)(JSGlobalObject*, JSObject* parent, JSObject* child);
    void (*pushObservableChild)(JSGlobalObject*, JSObject* parent, JSObject* child);
    void (*removeFromRequireCache)(JSGlobalObject*, JSString* path);
    JSValue (*requireSpecifier)(JSGlobalObject*, JSObject* requirer, JSValue specifier);
    JSValue (*resolveSpecifier)(JSGlobalObject*, JSObject* requirerOrImportMeta, JSValue specifier, ResolvedSpecifierForm);
    void (*captureSyntheticExports)(JSGlobalObject*, SyntheticExportRule, JSValue value, Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    // What require() returns for a built-in module that has no JavaScript source. Null in a host without such modules.
    JSValue (*requireNativeModule)(JSGlobalObject*, JSString* specifier);
    // What an import of such a module gets. The host lists the exports itself: they need not be the properties of what require() returns. Null in a host without such modules.
    void (*captureNativeModuleExports)(JSGlobalObject*, uint32_t registryID, Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    // The object that `require.cache` answers in every module of the realm. Null in a host without one.
    JSObject* (*requireCache)(JSGlobalObject*);
};

const ModuleHost& moduleHostOf(JSGlobalObject*);

JS_EXPORT_PRIVATE std::optional<ModuleID> findModule(StringView publicPath);
JS_EXPORT_PRIVATE StringView modulePath(ModuleID);
JS_EXPORT_PRIVATE bool isCommonJS(ModuleID);
JS_EXPORT_PRIVATE bool entryPointIsCommonJS(JSGlobalObject*);
JS_EXPORT_PRIVATE void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>&);
// Code: what the host would have to interpret or link at run time. Data files load in every executable.
JS_EXPORT_PRIVATE bool refusesCodeOutsideImage(JSGlobalObject*);
// The host calls this with every name that a function object of the program is given to load: require, module.require, Module._load, process.getBuiltinModule.
// It throws in a program whose build followed every name, whatever this one resolves to: no proof in the code allows for a module that is found by name.
// A specifier that names a module the build was told is external admits code outside the image from then on.
JS_EXPORT_PRIVATE void willLoadByName(JSGlobalObject*, JSValue specifier);

JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecord(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isCompiledModuleRecord(AbstractModuleRecord*);
JS_EXPORT_PRIVATE JSPromise* importModule(JSGlobalObject*, ModuleID, std::optional<int64_t> referrerAsyncOrder);
JS_EXPORT_PRIVATE void evaluateModuleSynchronously(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* moduleNamespace(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSValue requireModule(JSGlobalObject*, ModuleID, JSObject* commonJSModule);
JS_EXPORT_PRIVATE void evaluateCommonJSBody(JSGlobalObject*, ModuleID, JSObject* commonJSModule);

JS_EXPORT_PRIVATE bool isEvaluated(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* namespaceIfEvaluated(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void forEachEvaluatedModule(JSGlobalObject*, const ScopedLambda<void(ModuleID)>&);

// The host calls one of these three before it changes what a later require() answers. They throw where the build saw no such change:
// the code counts on the module that each constant name stood for, and on a body that runs once.
JS_EXPORT_PRIVATE void evictModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willSetRequireCacheEntry(JSGlobalObject*, ModuleID, JSValue entry);
JS_EXPORT_PRIVATE void willChangeRequireOverride(JSGlobalObject*, RequireOverride, bool isOverridden);
JS_EXPORT_PRIVATE void didObserveModuleChildren(JSGlobalObject*);

JS_EXPORT_PRIVATE bool isModuleNamespace(JSCell*);
JS_EXPORT_PRIVATE bool hasESModuleMarker(JSCell* moduleNamespace);
JS_EXPORT_PRIVATE void setESModuleMarker(JSCell* moduleNamespace, bool);

JS_EXPORT_PRIVATE void setVerbose(bool);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
