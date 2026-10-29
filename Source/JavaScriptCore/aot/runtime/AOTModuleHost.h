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
    // What require() keeps of a file that it has answered and that is no CommonJS file. The object has no `require`, and the engine stores the answer under `exports`.
    IsNoCommonJSFile = 1 << 2,
    // `module.loaded` stays false.
    IsESModule = 1 << 3,
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
    // What lies under `resolve` on the `require` of the module object, if it does what the host put there does, or else another function that does. Null in a host whose `require` has no such member.
    JSCell* (*resolveFunctionOf)(JSGlobalObject*, JSObject* requirer, JSCell* heldOrNull);
    // The program has registered something with the host that answers to the name of a native module in its place. Null in a host where nothing can be registered.
    bool (*replacesNativeModule)(JSGlobalObject*, const String& path);
    // Callbacks wait in a queue of the host's own that it empties between jobs, as process.nextTick() fills. Null in a host that has no such queue.
    bool (*hasQueuedTicks)(JSGlobalObject*);
    // The host loads the module as the one import of a module of its own, the main module. Null in a host that has no such module.
    bool (*isImportOfMainModule)(JSGlobalObject*, ModuleID);
    // The body of the main module.
    void (*runMainModule)(JSGlobalObject*, ModuleID entryPoint);
    // The error of the host's resolver for a specifier that it does not find. Null in a host whose resolver has no error of its own.
    JSValue (*createModuleNotFoundError)(JSGlobalObject*, const String& specifier, const String& importer, bool isForRequire);
};

const ModuleHost& moduleHostOf(JSGlobalObject*);

JS_EXPORT_PRIVATE std::optional<ModuleID> findModule(StringView publicPath);
// The image holds the file as something that only an import with an attribute gets.
JS_EXPORT_PRIVATE bool holdsFormThatAttributeAsksFor(StringView publicPath);
// False: both functions above return false for every path that starts with `start`.
JS_EXPORT_PRIVATE bool mayHoldPathStartingWith(StringView start);
JS_EXPORT_PRIVATE String modulePath(ModuleID);
// The URL of the source provider that the engine would run the module from. A text that the program evaluates, `new Function(text)`, has none.
String sourceProviderURL(ModuleID);
JS_EXPORT_PRIVATE bool entryPointIsCommonJS(JSGlobalObject*);
JS_EXPORT_PRIVATE bool isESModule(ModuleID);
// Of what findModule() answers: the host makes the module, and the image holds its number alone.
JS_EXPORT_PRIVATE bool isNativeModule(ModuleID);
// A realm that the program creates holds no compiled module. The host runs the modules of its library there as it does without an image.
JS_EXPORT_PRIVATE bool leavesModuleToHost(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>&);
// The path of each program file with code, in any image in use, that starts with `start`. Data files are looked up on disk. A file in several images is reported once per image. Callable from any thread.
JS_EXPORT_PRIVATE void forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>&);
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
// The host calls this with every name that a function object of the program is given to load: require, module.require, Module._load, process.getBuiltinModule.
JS_EXPORT_PRIVATE void willLoadByName(JSGlobalObject*, JSValue specifier, LoadByName = LoadByName::Require);
// The host calls this with the file of the image that such a name has resolved to, before it looks into a cache of its own. It throws for a file that is no entry point.
JS_EXPORT_PRIVATE void willAnswerWithModule(JSGlobalObject*, ModuleID, LoadByName = LoadByName::Require);

JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecord(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isCompiledModuleRecord(AbstractModuleRecord*);
JS_EXPORT_PRIVATE JSPromise* importModule(JSGlobalObject*, ModuleID, std::optional<int64_t> referrerAsyncOrder);
JS_EXPORT_PRIVATE void evaluateModuleSynchronously(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* moduleNamespace(JSGlobalObject*, ModuleID);
// False only if the namespace object of the module has no such name. Creates nothing.
JS_EXPORT_PRIVATE bool mayExport(JSGlobalObject*, ModuleID, UniquedStringImpl* name);
// The referrer is the file of the image whose require() the host answers.
JS_EXPORT_PRIVATE JSValue requireModule(JSGlobalObject*, ModuleID, std::optional<ModuleID> referrer = std::nullopt);

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
// Where threads are ending, the caller can wait for their room, so the new thread is refused the last of the room that is left now.
JS_EXPORT_PRIVATE PagesForNewThread tryReservePagesForNewThread(bool threadsAreEnding);
// The first thread takes its own: askForAddressSpaceToStart() has asked for the rest.
JS_EXPORT_PRIVATE PagesForNewThread tryReservePagesForFirstThread();
JS_EXPORT_PRIVATE void releasePagesForNewThread(void* pages);
// On the new thread, before it has a VM.
JS_EXPORT_PRIVATE void adoptPagesForNewThread(void* pages);
// On the new thread, when it ends: returns the pages if no VM took them. A VM takes all of them and returns them when it ends.
JS_EXPORT_PRIVATE void releaseUnusedPagesOfThread();
// On the new thread, before the first statement of the program runs, or once it has run code that is not compiled, or where it ends. Until then room is kept for what the thread has yet to take.
JS_EXPORT_PRIVATE void didStartThread(VM*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
