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
class VM;

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
    // The object between every `require` and Function.prototype. Null in a host that has none.
    JSObject* (*requirePrototype)(JSGlobalObject*);
    // A graph has come to a module with a mock whose factory has not run. Null in a host without mocks.
    void (*runMock)(JSGlobalObject*, ModuleID);
    bool (*loadsSynchronously)(JSGlobalObject*, ModuleID);
    // Calls deliverTextThatWaits() from its event loop, once for each call: in a later turn, behind every job that waits, and with every job run between two of them. Null in a host without an event loop.
    void (*callInLaterTurn)(JSGlobalObject*);
    // Differs where the file was written since: the build has asked the same of each file that the run may read again. Null in a host that reads no code at run time.
    // The path is UTF-8, and a zero follows it.
    uint64_t (*stampOfFile)(std::span<const char> path);
    // The program has registered something with the host that loads the file at the path in its own way. Asked about a name that the run time has resolved. Null in a host where nothing can be registered.
    bool (*loadsFileItself)(JSGlobalObject*, const String& path);
    // An import() in compiled code has begun. The promise is the loader's own where the loader is asked, or else the one that the program gets. The path is null for a file that nothing may mock. Null in a host where nothing waits for imports.
    void (*didStartImport)(JSGlobalObject*, JSPromise*, const String& path);
    // Factories that runMock() has called have answered promises that have not settled: a promise that is fulfilled once each has settled and its mock is laid, and rejected with what one has thrown.
    // Null where none waits, and in a host without mocks.
    JSPromise* (*factoriesThatWait)(JSGlobalObject*);
};

const ModuleHost& moduleHostOf(JSGlobalObject*);

JS_EXPORT_PRIVATE std::optional<ModuleID> findModule(StringView publicPath, unsigned hashOfPath);
inline std::optional<ModuleID> findModule(StringView publicPath) { return findModule(publicPath, publicPath.hash()); }
// The image holds the file as something that only an import with an attribute gets.
JS_EXPORT_PRIVATE bool holdsFormThatAttributeAsksFor(StringView publicPath);
// False: both functions above return false for every path that starts with `start`.
JS_EXPORT_PRIVATE bool mayHoldPathStartingWith(StringView start);
JS_EXPORT_PRIVATE bool isLeftToCodeOutsideImage(StringView specifier, StringView directoryOfImporter);
JS_EXPORT_PRIVATE String modulePath(ModuleID);
// What a stack trace names: modulePath() without the query that the module was asked for with.
String pathOfFileOfModule(ModuleID);
// The URL of the source provider that the engine would run the module from. A text that the program evaluates, `new Function(text)`, has none.
String sourceProviderURL(ModuleID);
JS_EXPORT_PRIVATE bool entryPointIsCommonJS(JSGlobalObject*);
JS_EXPORT_PRIVATE bool isESModule(ModuleID);
JS_EXPORT_PRIVATE bool isDataFile(ModuleID);
// A file of a test run that an executable holds too: ModuleFlags::FindsBuiltinsReadOnly. Nothing for a module of Bun's own code, which writes for whoever has called it.
std::optional<bool> findsBuiltinsReadOnly(ModuleID);
// The loader of `bun run` would have the file in its registry: an import of it has settled, or a module that imports it has been loaded.
JS_EXPORT_PRIVATE bool isHeldForImport(JSGlobalObject*, ModuleID);
// Of what findModule() answers: the host makes the module, and the image holds its number alone.
JS_EXPORT_PRIVATE bool isNativeModule(ModuleID);
// A module of Bun's own code.
JS_EXPORT_PRIVATE bool isOfLibrary(ModuleID);
JS_EXPORT_PRIVATE bool mayHoldEntryOfProgramInRequireCache(ModuleID);
// A mock stands in place of a module of the library, and the loader of the host has it.
JS_EXPORT_PRIVATE bool hostAnswersInPlaceOf(JSGlobalObject*, ModuleID, LoadByName);
// Code of the program has required the module of the library, and the engine keeps what the next require() answers: what a mock has stored there since, too.
JS_EXPORT_PRIVATE bool isInRequireCache(JSGlobalObject*, ModuleID);
// A realm that the program creates holds no compiled module. The host runs the modules of its library there as it does without an image.
// A file that the host starts by itself, as a test runner starts a test file, stays compiled whatever it imports.
enum class StartedBy : uint8_t { Code, Host };
JS_EXPORT_PRIVATE bool leavesModuleToHost(JSGlobalObject*, ModuleID, StartedBy = StartedBy::Code);
// The host calls this when it goes on to load such a module itself.
JS_EXPORT_PRIVATE void willLoadCopyFromDisk(JSGlobalObject*, ModuleID, LoadByName);
// ModuleHost::loadsSynchronously: the first load of a file decides for the VM. Where a run goes through several processes, one after another, the host hands what one has decided to the next.
JS_EXPORT_PRIVATE void forEachFirstLoad(VM&, const ScopedLambda<void(const String& path, bool wasSynchronous)>&);
JS_EXPORT_PRIVATE void noteFirstLoadInEarlierProcess(VM&, const String& path, bool wasSynchronous);
// ModuleHost::callInLaterTurn.
JS_EXPORT_PRIVATE void deliverTextThatWaits(VM&);
// The loader fetches the module for an import() in another image. Asked before the record is made.
JS_EXPORT_PRIVATE bool textOfFetchedModuleComesInLaterTurn(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void holdTextOfFetchedModule(JSGlobalObject*, ModuleID, JSPromise* fetched, AbstractModuleRecord*);
JS_EXPORT_PRIVATE void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>&);
// The files that have run in a program that counts how often its places run: what the compiler said of the file, and the counts by the compiler's numbers. Places past the end have not run.
JS_EXPORT_PRIVATE void forEachCountedFile(VM&, const ScopedLambda<void(const String& description, std::span<const uint64_t> counts)>&);
// The path of each program file with code, in any image in use, that starts with `start`. Data files are looked up on disk. A file in several images is reported once per image. Callable from any thread.
JS_EXPORT_PRIVATE void forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>&);
// For a build above the images, the first of them first, which does not run them: whether a file with code at the path is a module of one of them. The bytes stay for as long as the process. Any thread may ask.
JS_EXPORT_PRIVATE bool imagesHoldFile(std::span<const std::span<const uint8_t>> images, StringView path);
// Code: what the host would have to interpret or link at run time. Data files load in every executable.
JS_EXPORT_PRIVATE bool refusesCodeOutsideImage(JSGlobalObject*, StringView path);
// The host loads the text of a file of the program once more, as an instance of its own. It names the files of the program as the compiled file does.
JS_EXPORT_PRIVATE void willRunCopyFromDisk(JSGlobalObject*);
// A file whose loader answers an object of the host, a database or a page.
JS_EXPORT_PRIVATE bool refusesHostObjectsOutsideImage(JSGlobalObject*);

JS_EXPORT_PRIVATE JSValue moduleExports(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void setModuleExports(JSGlobalObject*, ModuleID, JSValue);
// What `require.main` answers while `Bun.main` is the path of this file. Null where `bun run` has no module object for it yet.
JS_EXPORT_PRIVATE JSObject* mainModuleObject(JSGlobalObject*, ModuleID);
// Comes before a store of another value into `Bun.main`. Throws where the build saw no statement that stores there: `require.main === module` and `import.meta.main` are decided in the code,
// and only an entry point has a module object for `require.main` to answer. In a test run the runner stores there for each test file, and a test may too.
JS_EXPORT_PRIVATE void willStorePathOfMainModule(JSGlobalObject*);
// Of the file whose require() came to the module first. Null where the program recorded none.
JS_EXPORT_PRIVATE JSObject* parentModuleObject(JSGlobalObject*, ModuleID);
// Calls the functor with the module object of each CommonJS file that the module has required since the last call, in the order of the first require() of each.
JS_EXPORT_PRIVATE bool forEachNewChild(JSGlobalObject*, ModuleID, const ScopedLambda<void(JSObject*)>&);
// The host calls this with every name that a function object of the program is given to load: require, module.require, Module._load, process.getBuiltinModule.
JS_EXPORT_PRIVATE void willLoadByName(JSGlobalObject*, JSValue specifier, LoadByName = LoadByName::Require);
// In the host's resolve step: an import() of compiled code asks, which does not come by the host's own hook for import().
JS_EXPORT_PRIVATE bool isResolvingImportCall(VM&);
// In ModuleHost::runMock: a require() has come to the module, so what the mock loads is loaded at once.
JS_EXPORT_PRIVATE bool requireRunsMocks(VM&);
// A module outside the image asked a compiled module for a name that the build left out of its table.
using MissingExportListener = void (*)(const String& name, const String& exporter, const String& importer);
JS_EXPORT_PRIVATE void setMissingExportListener(MissingExportListener);
// The host calls this with the file of the image that such a name has resolved to, before it looks into a cache of its own. It throws for a file that is no entry point.
JS_EXPORT_PRIVATE void willAnswerWithModule(JSGlobalObject*, ModuleID, LoadByName = LoadByName::Require);
class Modules;
// The file whose import or require() the host is answering. A refusal names it.
class AskerOfModule {
    WTF_MAKE_NONCOPYABLE(AskerOfModule);
public:
    JS_EXPORT_PRIVATE AskerOfModule(JSGlobalObject*, const String& path);
    JS_EXPORT_PRIVATE ~AskerOfModule();

private:
    Modules* m_modules;
    const String* m_outer;
};

JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecord(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isCompiledModuleRecord(AbstractModuleRecord*);
// Of the namespace object of a compiled module: the record to ask by AbstractModuleRecord::resolveExport(). Null for any other cell.
JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecordOfNamespace(JSGlobalObject*, JSCell* moduleNamespace);
JS_EXPORT_PRIVATE JSPromise* importModule(JSGlobalObject*, ModuleID, std::optional<int64_t> referrerAsyncOrder);
JS_EXPORT_PRIVATE void evaluateModuleSynchronously(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* moduleNamespace(JSGlobalObject*, ModuleID);
// False only if the namespace object of the module has no such name. Creates nothing.
JS_EXPORT_PRIVATE bool mayExport(JSGlobalObject*, ModuleID, UniquedStringImpl* name);
// The referrer is the file of the image whose require() the host answers.
JS_EXPORT_PRIVATE JSValue requireModule(JSGlobalObject*, ModuleID, std::optional<ModuleID> referrer = std::nullopt);
// In front of requireModule(), where the host has the module object of the file that requires. The engine keeps it where that file is no module of the image that holds the required one.
JS_EXPORT_PRIVATE void willRequireModule(JSGlobalObject*, ModuleID, std::optional<ModuleID> referrer, JSObject* moduleObjectOfReferrer);
JS_EXPORT_PRIVATE void didMakeModuleObjectForRequire(JSGlobalObject*, ModuleID, JSObject* moduleObject);
// Behind a require() that has returned. Both numbers are the host's.
JS_EXPORT_PRIVATE void didRequireModule(JSGlobalObject*, ModuleID, ModuleID referrer);

// A module of the image runs at most once and is in no module cache, and the code calls the module that each constant name stood for in the build.
JS_EXPORT_PRIVATE void willSetRequireCacheEntry(JSGlobalObject*, ModuleID, JSValue entry);
// The program stores an entry under a name of the Node.js module without `node:`, where the host's require() looks before it goes to the registry.
// Throws if the build, which read no such key, compiled a require() of the program under such a name for the module.
JS_EXPORT_PRIVATE void willSetRequireCacheEntryUnderName(JSGlobalObject*, ModuleID, const String& name);
// The host admits code from disk: the next load of the file takes it from there, as a new instance. An executable has its files in no cache.
JS_EXPORT_PRIVATE void didDeleteRequireCacheEntry(JSGlobalObject*, ModuleID);
// A test runner hands each test file what its setup code has loaded: where a test file resets the modules, the next one finds those instances again.
// The host calls the first two round each piece of that code, and what runs between them is kept; the third for each file that a reset takes out, as it would call
// didDeleteRequireCacheEntry(); and the fourth between two test files: it names each file that is back, of which the host may hold a copy from disk.
JS_EXPORT_PRIVATE void willRunCodeThatKeepsFiles(JSGlobalObject*);
JS_EXPORT_PRIVATE void keepFilesThatHaveRun(JSGlobalObject*);
JS_EXPORT_PRIVATE void didResetModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void bringBackKeptFiles(JSGlobalObject*, const WTF::Function<void(const String&)>&);
// The file was taken out of the program and is not back: whoever has bound it keeps it as it is, and every load of it is the host's.
JS_EXPORT_PRIVATE bool isEvictedFile(JSGlobalObject*, ModuleID);
// Whether didDeleteRequireCacheEntry() would throw: the file has run, and the code that imports it was compiled for a file that loads once.
JS_EXPORT_PRIVATE bool refusesToLoadAgain(JSGlobalObject*, ModuleID);
// The host admits code from disk, and the program stores a falsy value over the entry, which the host's require() takes for no entry. The store leaves the modules that imports have made
// where they are, and the host's require() looks there next: a file that an import has loaded stays.
JS_EXPORT_PRIVATE void willClearRequireCacheEntry(JSGlobalObject*, ModuleID);
// The host admits code from disk, and the program stores an entry of its own, which the host's require() answers with from then on. Throws for a file that the build bound for good.
JS_EXPORT_PRIVATE void willReplaceRequireCacheEntry(JSGlobalObject*, ModuleID);
// The host admits code from disk, and the program has given it something that loads the file in another way than the build did. A file that has run stays.
// False: the code that imports the file was compiled for the file as the build read it.
JS_EXPORT_PRIVATE bool willLoadFileInAnotherWay(JSGlobalObject*, ModuleID);
// The host is about to load an entry point once more in the same realm, as `bun test --rerun-each` does: its body runs again, over what it imports as that stands.
// Only an ES module does, and only in an image that was built for it: ModuleFlags::RunsAgain.
JS_EXPORT_PRIVATE void willRunEntryPointAgain(JSGlobalObject*, ModuleID);
// Also a file that is still running, where `bun run` lists it by then: Modules::isListedWhileItRuns().
JS_EXPORT_PRIVATE bool isEvaluatedFile(JSGlobalObject*, ModuleID);
// The module object that require.cache shows for a file that isEvaluatedFile().
JS_EXPORT_PRIVATE JSObject* requireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void forEachEvaluatedFile(JSGlobalObject*, const WTF::Function<void(const String&)>&);
// The files that wait for a top-level await of their own, each with its turn among all the modules that evaluate asynchronously.
// A file that another imports by a statement is in no registry of the loader.
JS_EXPORT_PRIVATE void forEachFileAwaitingAtTopLevel(JSGlobalObject*, const WTF::Function<void(int64_t turn, const String& key)>&);
// In the order of the first require() of each.
JS_EXPORT_PRIVATE void forEachLibraryModuleInRequireCache(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE std::optional<ModuleID> findLibraryModuleInRequireCache(JSGlobalObject*, const String& name);
JS_EXPORT_PRIVATE void willChangeRequireOverride(JSGlobalObject*, RequireOverride, bool isOverridden);
// The host admits code from disk, and the program stores a loader under the extension: the host's require() hands it every file of that extension that has not run. Throws for a file that the build bound for good.
JS_EXPORT_PRIVATE void willStoreLoaderOfExtension(JSGlobalObject*, StringView extension);

JS_EXPORT_PRIVATE bool isModuleNamespace(JSCell*);
// For the test runner: stores the value in the variable behind the export, where the module itself and every importer read it.
// False where the build saw no call that replaces the export: code that uses it may not read the variable.
JS_EXPORT_PRIVATE bool tryReplaceExport(JSCell* moduleNamespace, UniquedStringImpl* name, JSValue);
// The build saw a call that mocks the module: every export of it can be replaced.
JS_EXPORT_PRIVATE bool mayBeMocked(JSGlobalObject*, ModuleID);
// The module has not begun to load: the factory of the mock runs when a graph comes to it (ModuleHost::runMock).
JS_EXPORT_PRIVATE bool hasBegunToLoad(JSGlobalObject*, ModuleID);
// Under `bun test` the body of a module that is linked when it is mocked runs then and there: left for later, it would assign over what the mock stores.
JS_EXPORT_PRIVATE void runBodyOfLinkedModuleAheadOfMock(JSGlobalObject*, ModuleID);
// `a.txt?text` is a form of `a.txt`: what an import attribute makes of the file. A mock of the path replaces the form that has been loaded, and waits for all of them if none has. `file`: the module under the path itself, if the image holds it.
JS_EXPORT_PRIVATE std::optional<ModuleID> formThatMockFinds(JSGlobalObject*, StringView path, std::optional<ModuleID> file);
JS_EXPORT_PRIVATE Vector<ModuleID, 2> formsThatAttributesAskFor(JSGlobalObject*, StringView path);
// AfterFailure::AsksAgain: a factory that takes importOriginal. `bun test` loads the original first and lays the mock over it, so where the factory fails the loader keeps no record with the
// error: the next load asks the factory again, and whoever imports the module loads then.
enum class AfterFailure : bool { KeepsError, AsksAgain };
JS_EXPORT_PRIVATE void deferMock(JSGlobalObject*, ModuleID, AfterFailure = AfterFailure::KeepsError);
// The mock has ended: its test file is over, or the program has taken it back. The next load finds the module itself, and whoever has imported a module of the library reads its own names again.
JS_EXPORT_PRIVATE void didEndMock(JSGlobalObject*, ModuleID);
// What a file of the program that has run exported before a mock was laid over it: `module.exports` of a CommonJS file, a new object with the names of an ES module.
// So too while the factory runs and nothing is laid yet. Empty for a file that never ran: the mock stands in its place.
JS_EXPORT_PRIVATE JSValue exportsBeforeMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void runMocksBelow(JSGlobalObject*, ModuleID);
// Behind runMocksBelow() that threw: it was the factory of the module itself, and an import statement asks for the module.
JS_EXPORT_PRIVATE bool mockFailedBehindImportStatement(JSGlobalObject*, ModuleID);
// The loader may answer for a name with another module than before: the next import() of a name that is left to it asks again.
JS_EXPORT_PRIVATE void didChangePlugins(JSGlobalObject*);
// A mock that comes ahead of the load of a module stands in place of it. The body of the module does not run. Every import of a name, the namespace object and whoever links to the module ask `mock`,
// by the object's own methods, at each use: nothing is copied. require() answers `whatRequireAnswers`. Null lets go of the mock: nothing of the module was filled, so it is as it was, and loads when it is next asked for.
// False: the mock is not taken, and goes by willMockModule(): over a built-in module that an import of the program had loaded when its first mock was made, the values are laid as over a file that has run.
JS_EXPORT_PRIVATE bool setMock(JSGlobalObject*, ModuleID, JSObject* mock, JSValue whatRequireAnswers);
// A later mock of such a module takes the place of the earlier one.
JS_EXPORT_PRIVATE bool hasMockInPlace(JSGlobalObject*, ModuleID);
// didEndMock() leaves a mock that stands in place of an ES module to whoever was linked against it, until a load comes to the module. The test file ends: nobody reads it any more.
JS_EXPORT_PRIVATE void letGoOfMocksThatEnded(JSGlobalObject*);
// The `mock` that the namespace object of a compiled module asks now. Null: no mock stands in place of its module, or the cell is no such object.
JS_EXPORT_PRIVATE JSObject* mockInPlaceBehind(JSCell* moduleNamespace);
// The build has seen that a mock may name the module, and its importers look for one. No mock stands in place of any other module: setMock() is not for it.
JS_EXPORT_PRIVATE bool hasWordOfMock(JSGlobalObject*, ModuleID);
// Comes before the mockExport() of each name. A module that has not run yet never runs: neither its body nor, for its sake, what it imports. require() answers a CommonJS file with the object.
// The body of a module of the library still runs for the library itself, which no mock reaches.
JS_EXPORT_PRIVATE void willMockModule(JSGlobalObject*, ModuleID, JSObject* exports);
JS_EXPORT_PRIVATE void didMockModule(JSGlobalObject*, ModuleID);
// Stores one export of the mock. A name that the module does not export is passed over.
JS_EXPORT_PRIVATE void mockExport(JSGlobalObject*, ModuleID, UniquedStringImpl* name, JSValue);
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
