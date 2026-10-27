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
#include <limits>
#include <optional>
#include <span>
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
class ScriptFetchParameters;

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
    // A require() in the file of the module object has returned a CommonJS file: for the first time, and forEachNewChild() names the file, or once more.
    void (*showChildren)(JSGlobalObject*, JSObject* module, JSObject* requiredAgain);
    // The module of the image that an import of the path with these attributes gets. The build has named each module by the rules of the host. Null in a host where attributes choose nothing.
    std::optional<ModuleID> (*findModuleWithAttributes)(JSGlobalObject*, const String& path, const ScriptFetchParameters&);
};

const ModuleHost& moduleHostOf(JSGlobalObject*);

JS_EXPORT_PRIVATE std::optional<ModuleID> findModule(StringView publicPath);
// The image holds the file as something that only an import with an attribute gets.
JS_EXPORT_PRIVATE bool holdsFormThatAttributeAsksFor(StringView publicPath);
JS_EXPORT_PRIVATE String modulePath(ModuleID);
// The URL of the source provider that the engine would run the module from. A text that the program evaluates, `new Function(text)`, has none.
String sourceProviderURL(ModuleID);
JS_EXPORT_PRIVATE bool entryPointIsCommonJS(JSGlobalObject*);
JS_EXPORT_PRIVATE bool isESModule(ModuleID);
// A realm that the program creates holds no compiled module. The host runs the modules of its library there as it does without an image.
JS_EXPORT_PRIVATE bool leavesModuleToHost(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>&);
// Code: what the host would have to interpret or link at run time. Data files load in every executable.
JS_EXPORT_PRIVATE bool refusesCodeOutsideImage(JSGlobalObject*, StringView path);
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

// Whether the limit on the address space of the process leaves what a program of this size maps until its first statement runs. It allocates nothing: the host asks before its allocator starts.
struct AddressSpaceToStart {
    size_t needed { 0 };
    size_t left { 0 };
    // The allocator reserves address space in steps: the largest of them that leaves enough for the rest, or 0.
    size_t largestStepOfAllocator { std::numeric_limits<size_t>::max() };
    bool isRefused { false };
};
JS_EXPORT_PRIVATE AddressSpaceToStart askForAddressSpaceToStart(size_t sizeOfProgram, std::span<const size_t> stepsOfAllocatorFromLargest);

// The address space for the tables that a thread reserves when it creates its VM and its instance. The thread that starts it takes the pages, so it is there that a refusal is heard.
// Only where the address space of the process is limited: elsewhere there is nothing to take, and the pages are null.
struct PagesForNewThread {
    void* pages { nullptr };
    bool isRefused { false };
};
// Also makes sure that the allocator has room for what the thread takes as it starts, and that there is room for its stack.
JS_EXPORT_PRIVATE PagesForNewThread tryReservePagesForNewThread();
// The first thread takes its own: askForAddressSpaceToStart() has asked for the rest.
JS_EXPORT_PRIVATE PagesForNewThread tryReservePagesForFirstThread();
JS_EXPORT_PRIVATE void releasePagesForNewThread(void* pages);
// On the new thread, before it has a VM.
JS_EXPORT_PRIVATE void adoptPagesForNewThread(void* pages);
// On the new thread, once it has its instance, or ends without one.
JS_EXPORT_PRIVATE void releaseUnusedPagesOfThread();

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
