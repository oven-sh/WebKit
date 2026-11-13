/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTHostFunctionSources.h"
#include "AOTVector.h"
#include <JavaScriptCore/AOTRecordTypes.h>
#include <JavaScriptCore/ArgList.h>
#include <JavaScriptCore/Identifier.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <JavaScriptCore/ScriptFetcher.h>
#include <limits>
#include <optional>
#include <span>
#include <wtf/IterationStatus.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/StringView.h>

namespace JSC {

class AbstractModuleRecord;
class CallFrame;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSPromise;
class JSString;
class ScriptFetchParameters;
class SourceOrigin;
class VM;

namespace AOT {

struct Instance;
struct MockOperations;

struct ModuleObjects {
    JSObject* module { nullptr };
    JSValue require;
};

enum class ModuleObjectTrait : uint8_t {
    IsMain = 1 << 0,
    HasExportsVariable = 1 << 1,
    IsNotCommonJSModule = 1 << 2,
    IsRequiredESModule = 1 << 3,
};

enum class ResolvedSpecifierForm : uint8_t { Path, PathForRequire, URL };
enum class LoadKind : uint8_t { Require, Import };
enum class OriginalRun : uint8_t { FromSource, PrivateCopy, Failed, SecondInstance, MockedDuringImport, MockAddsNames, LoadsItsImporter, LoadFailsToLink, ExportsNamesOfMocks, RunsAmongLoads, RequireRejected, AwaitsMockedAgain, ResetWhileAwaiting, BuiltinMockChanged, TestFilesRunAgain };
enum class RequireOverride : uint8_t {
    ResolveFilename = 1 << 0,
    RequireFunction = 1 << 1,
    ExtensionLoader = 1 << 2,
};

enum class PendingFactoryRequester : uint8_t { ImportCall, ImportCallAfterSource, DetachedLoad, Loader, Require, RejectedRequire };

enum class PluginResolveResult : uint8_t { Declined, Resolved, Threw };

struct ModuleHost {
    void (*createModuleObject)(JSGlobalObject*, JSString* path, ModuleID, OptionSet<ModuleObjectTrait>, ModuleObjects&);
    void (*createRequirer)(JSGlobalObject*, JSObject* importMeta, ModuleID, ModuleObjects&);
    JSValue (*requireSpecifier)(JSGlobalObject*, JSObject* requirer, JSValue specifier);
    JSValue (*resolveSpecifier)(JSGlobalObject*, JSObject* requirerOrImportMeta, JSValue specifier, ResolvedSpecifierForm);
    JSObject* (*captureSyntheticExports)(JSGlobalObject*, SyntheticExportRule, JSValue value, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    JSValue (*requireNativeModule)(JSGlobalObject*, JSString* specifier);
    JSObject* (*captureNativeModuleExports)(JSGlobalObject*, uint32_t registryID, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    JSObject* (*requireCache)(JSGlobalObject*);
    JSObject* (*requireExtensions)(JSGlobalObject*);
    JSObject* (*createAsyncModuleRequiredError)(JSGlobalObject*, const String& path);
    JSValue (*createLoadError)(JSGlobalObject*, const String& path);
    void (*appendModuleChildren)(JSGlobalObject*, JSObject* module, JSObject* childOrNull);
    std::optional<ModuleID> (*findModuleWithAttributes)(JSGlobalObject*, const String& path, const ScriptFetchParameters&);
    JSCell* (*ensureResolveFunction)(JSGlobalObject*, JSObject* requirer, JSCell* cachedOrNull);
    bool (*replacesNativeModule)(JSGlobalObject*, const String& path);
    bool (*hasQueuedTicks)(JSGlobalObject*);
    bool (*isMainModuleImport)(JSGlobalObject*, ModuleID);
    void (*didEvaluateMainModule)(JSGlobalObject*, ModuleID entryPoint);
    JSValue (*createModuleNotFoundError)(JSGlobalObject*, const String& specifier, const String& importer, bool isForRequire);
    JSObject* (*requirePrototype)(JSGlobalObject*);
    bool (*runMock)(JSGlobalObject*, ModuleID);
    bool (*loadsSynchronously)(JSGlobalObject*, ModuleID);
    bool (*hasMatchingLoadPlugin)(JSGlobalObject*, const String& path);
    bool (*hasResolvePlugins)(JSGlobalObject*);
    PluginResolveResult (*resolveWithPlugins)(JSGlobalObject*, const String& name, const String& importer, String& resolvedWithoutPlugins);
    String (*resolveWithoutPlugins)(JSGlobalObject*, const String& name, const String& importer);
    void (*beginOrEndPluginResolutions)(JSGlobalObject*, const ScopedLambda<bool(const String& importer)>* shouldCacheForLoader);
    const MockOperations* (*mockOperations)();
    uint64_t (*computeFileStamp)(std::span<const char> path);
    bool (*shouldBypassMock)(JSGlobalObject*, const String& path, const String& importer, bool isImport);
    bool (*wasLinkedBeforeMock)(JSGlobalObject*, const ScopedLambda<String()>& path, const ScopedLambda<String()>& importer);
    bool (*isMockHiddenFromRequirer)(JSGlobalObject*, const String& path, const String& requirer);
    JSPromise* (*nextPendingFactory)(JSGlobalObject*, const ScopedLambda<bool(ModuleID)>& isInGraph, PendingFactoryRequester, JSValue& state);
    std::optional<bool> (*isInPreload)(JSGlobalObject*);
    bool (*reinstallMocks)(JSGlobalObject*, const ScopedLambda<bool(ModuleID)>& isReached);
    uint64_t (*scheduleFetchFulfillment)(JSGlobalObject*);
    void (*didStartImport)(JSGlobalObject*, JSPromise*, const String& path, const ScopedLambda<BitVector()>& reachedModules);
    bool (*hasProgramSources)(JSGlobalObject*);
    bool (*isRequestedByPendingFactory)(JSGlobalObject*, ModuleID);
    bool (*hasSourceCache)(JSGlobalObject*);
    void (*crashWithReason)(std::span<const char> reason);
    DecompressSourceFunction decompressSource;
    uint64_t (*pendingFactoryCallID)(JSGlobalObject*, ModuleID, bool& shouldRejectEveryRequire);
    JSPromise* (*evaluateOriginalBeforeFactory)(JSGlobalObject*, ModuleID, bool& hasFactory);
    bool (*isLoadingUnderVitest)(JSGlobalObject*);
    void (*evictCachedSource)(JSGlobalObject*, const String& path);
    bool (*printsDataFileAsJavaScript)(JSGlobalObject*, const String& path);
    void (*didDiscardModuleEvaluation)(JSGlobalObject*, JSObject* module, bool willEvaluateAgain);
    bool (*isPatchPending)(JSGlobalObject*, const ScopedLambda<String()>& path);
    bool (*hasRunningFactory)(JSGlobalObject*);
    uint32_t (*willLoad)(JSGlobalObject*, ModuleID callerModuleIfKnown, ModuleID asked, LoadKind);
    bool (*isInPendingFactoryChain)(JSGlobalObject*, ModuleID file, ModuleID mocked);
    bool (*isRequiredByOwnFactory)(JSGlobalObject*, ModuleID required, ModuleID requirer);
    bool (*isFactoryCallPending)(JSGlobalObject*, uint64_t call);
    bool (*hasRegisteredMock)(JSGlobalObject*, const String& path);
    bool (*isAwaitingOriginalBody)(JSGlobalObject*, ModuleID);
    void (*didRunOriginal)(JSGlobalObject*, const String& path, OriginalRun);
    bool (*deferPatchToNextLoad)(JSGlobalObject*, ModuleID, bool isFactoryPending);
    bool (*runsTestFilesAgain)();
    JSValue (*requireOriginal)(JSGlobalObject*, ModuleID, bool isForOwnFactory);
    bool (*wasFetchedByTestFile)(JSGlobalObject*, const String& key);
    bool (*hasSynchronousFactory)(JSGlobalObject*, const String& mocked, bool isOriginalSynchronous);
    bool (*isSpy)(JSGlobalObject*, const String& mocked);
    JSPromise* (*sourceAfterRelease)(JSGlobalObject*, AbstractModuleRecord*, ModuleID);
    bool (*hasPendingPatch)(JSGlobalObject*, ModuleID);
    bool (*evictOriginal)(JSGlobalObject*, const String& mocked);
    bool (*requiresPatch)(JSGlobalObject*, const ScopedLambda<String()>& path, JSObject*& exports);
    bool (*isESModuleInNode)(JSGlobalObject*, const String& path);
};

const ModuleHost& moduleHostOf(JSGlobalObject*);

JS_EXPORT_PRIVATE const MockOperations* mockOperations();

JS_EXPORT_PRIVATE std::optional<ModuleID> findModule(StringView publicPath, unsigned hashOfPath);
inline std::optional<ModuleID> findModule(StringView publicPath) { return findModule(publicPath, publicPath.hash()); }
bool isOriginOfCompiledFile(const Instance&, const SourceOrigin&);
JS_EXPORT_PRIVATE bool hasAttributeVariant(StringView publicPath);
JS_EXPORT_PRIVATE bool mayContainPathWithPrefix(StringView start);
JS_EXPORT_PRIVATE bool isExternalSpecifier(StringView specifier, StringView importerDirectory);
JS_EXPORT_PRIVATE String pathBesideFile(StringView file, StringView written);
JS_EXPORT_PRIVATE String modulePath(ModuleID);
String moduleFilePath(ModuleID);
#define AOT_BUILD_FOLDER_PREFIX "./"
String moduleDisplayPath(ModuleID);
String sourceProviderURL(ModuleID);
JS_EXPORT_PRIVATE bool entryPointIsCommonJS(JSGlobalObject*);
JS_EXPORT_PRIVATE bool isESModule(ModuleID);
JS_EXPORT_PRIVATE bool isSynchronousESModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isDataFile(ModuleID);
JS_EXPORT_PRIVATE bool hasModuleBody(ModuleID);
std::optional<bool> hasReadOnlyBuiltins(ModuleID);
class FetcherOfTextHandedIn : public ScriptFetcher {
public:
    bool isMadeByFileOfProduct() const { return m_isMadeByFileOfProduct; }

protected:
    JS_EXPORT_PRIVATE explicit FetcherOfTextHandedIn(VM&);

private:
    bool m_isMadeByFileOfProduct;
};
enum class ModuleAliasKind : uint8_t { WithoutExtension, WithOtherExtension, AsDirectory };
JS_EXPORT_PRIVATE bool isImportedByAlias(ModuleID, ModuleAliasKind);
JS_EXPORT_PRIVATE bool hasBeenImported(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasBeenImportedSinceReset(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasPendingMockAfterReset(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isPatchedLibraryModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isNativeModule(ModuleID);
JS_EXPORT_PRIVATE bool isOfLibrary(ModuleID);
JS_EXPORT_PRIVATE bool mayHaveProgramCacheEntry(ModuleID);
JS_EXPORT_PRIVATE bool isReplacedByHost(JSGlobalObject*, ModuleID, LoadKind);
JS_EXPORT_PRIVATE bool isInRequireCache(JSGlobalObject*, ModuleID);
enum class StartedBy : uint8_t { Code, Host };
JS_EXPORT_PRIVATE bool delegatesModuleToHost(JSGlobalObject*, ModuleID, StartedBy = StartedBy::Code);
JS_EXPORT_PRIVATE void willLoadCopyFromDisk(JSGlobalObject*, ModuleID, LoadKind);
JS_EXPORT_PRIVATE void forEachFirstLoad(VM&, const ScopedLambda<void(const String& path, bool wasSynchronous)>&);
JS_EXPORT_PRIVATE void recordFirstLoadFromEarlierProcess(VM&, const String& path, bool wasSynchronous);
JS_EXPORT_PRIVATE void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>&);
JS_EXPORT_PRIVATE void forEachCountedFile(VM&, const ScopedLambda<void(const String& description, std::span<const uint64_t> counts)>&);
JS_EXPORT_PRIVATE void forEachFilePathWithPrefix(StringView start, const ScopedLambda<IterationStatus(const String&)>&);
JS_EXPORT_PRIVATE bool rejectsUncompiledCode(JSGlobalObject*, StringView path);
JS_EXPORT_PRIVATE bool wasModifiedSinceBuild(JSGlobalObject*, StringView path);
JS_EXPORT_PRIVATE bool willRunCopyFromDisk(JSGlobalObject*);
JS_EXPORT_PRIVATE bool rejectsUncompiledHostObjects(JSGlobalObject*);

JS_EXPORT_PRIVATE JSValue moduleExports(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void setModuleExports(JSGlobalObject*, ModuleID, JSValue);
JS_EXPORT_PRIVATE JSObject* mainModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willSetMainModulePath(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* parentModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* childModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool canExposeModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool forEachNewChild(JSGlobalObject*, ModuleID, const ScopedLambda<void(JSObject*)>&);
JS_EXPORT_PRIVATE void clearNewChildren(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willLoadByName(JSGlobalObject*, JSValue specifier, LoadKind = LoadKind::Require);
JS_EXPORT_PRIVATE bool isResolvingImportCall(VM&);
JS_EXPORT_PRIVATE bool isResolvingImportCallWithoutAttributes(VM&);
JS_EXPORT_PRIVATE bool requireRunsMocks(VM&);
JS_EXPORT_PRIVATE bool isCurrentRequireTarget(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isEvictedDataFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasOnlyBeenRequired(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willReplaceRequiredDataFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willImportEvictedDataOriginal(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void setMockedAfterEviction(JSGlobalObject*, ModuleID file, bool isMockedAfterEviction);
JS_EXPORT_PRIVATE bool awaitsGeneratedModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isRequiringAnotherModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE String requirerPath(JSGlobalObject*, ModuleID required);
JS_EXPORT_PRIVATE bool isCurrentImportTarget(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE std::optional<ModuleID> currentLoadTarget(JSGlobalObject*);
struct CurrentImportCall {
    ModuleID callerModule;
    ModuleID importTarget;
    uint32_t callSequenceNumber { 0 };
};
JS_EXPORT_PRIVATE std::optional<CurrentImportCall> currentImportCall(JSGlobalObject*);
JS_EXPORT_PRIVATE bool isMockedForImportCall(JSGlobalObject*, ModuleID mocked, ModuleID importer);
JS_EXPORT_PRIVATE void retainRecordForInFlightLoads(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isRetainedByInFlightLoads(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isInLoadGraph(JSGlobalObject*, ModuleID root, ModuleID);
enum class LoadGraphMode : uint8_t { RespectMocks, OriginalRoot, IgnoreMocks };
JS_EXPORT_PRIVATE BitVector modulesInLoadGraph(JSGlobalObject*, ModuleID root, LoadGraphMode = LoadGraphMode::RespectMocks);
JS_EXPORT_PRIVATE BitVector unfinishedModulesInLoadGraph(JSGlobalObject*, ModuleID root, LoadGraphMode, const BitVector& unsettledMocks);
JS_EXPORT_PRIVATE bool isCurrentImportTargetAfterWait(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSValue currentImportWaitState(JSGlobalObject*);
using MissingExportListener = void (*)(const String& name, const String& exporter, const String& importer);
JS_EXPORT_PRIVATE void setMissingExportListener(MissingExportListener);
JS_EXPORT_PRIVATE void checkModuleAccess(JSGlobalObject*, ModuleID, LoadKind = LoadKind::Require);
class Modules;
class ModuleRequesterScope {
    WTF_MAKE_NONCOPYABLE(ModuleRequesterScope);
public:
    JS_EXPORT_PRIVATE ModuleRequesterScope(JSGlobalObject*, const String& path);
    JS_EXPORT_PRIVATE ~ModuleRequesterScope();

private:
    Modules* m_modules;
    const String* m_previousRequester;
};
JS_EXPORT_PRIVATE const String& compiledCodeRequester();
JS_EXPORT_PRIVATE bool hasUncompiledRequester(JSGlobalObject*);
JS_EXPORT_PRIVATE void throwCompiledModuleAccessError(JSGlobalObject*, ModuleID, LoadKind);

JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecord(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isCompiledModuleRecord(AbstractModuleRecord*);
JS_EXPORT_PRIVATE bool isReleasedModuleRecord(AbstractModuleRecord*);
JS_EXPORT_PRIVATE std::optional<ModuleID> compiledModuleForRecord(JSGlobalObject*, AbstractModuleRecord*);
JS_EXPORT_PRIVATE AbstractModuleRecord* moduleRecordForNamespace(JSGlobalObject*, JSCell* moduleNamespace);
JS_EXPORT_PRIVATE JSPromise* importModule(JSGlobalObject*, ModuleID, std::optional<int64_t> referrerAsyncOrder);
JS_EXPORT_PRIVATE void evaluateModuleSynchronously(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* moduleNamespace(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool mayExport(JSGlobalObject*, ModuleID, UniquedStringImpl* name);
enum class ReadsExports : bool { No, Yes };
JS_EXPORT_PRIVATE std::optional<ModuleID> moduleFormOf(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE std::optional<ModuleID> loadedModuleForm(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSValue requireModule(JSGlobalObject*, ModuleID, std::optional<ModuleID> referrer = std::nullopt, ReadsExports = ReadsExports::Yes);
JS_EXPORT_PRIVATE bool hasCommonJSEvaluationStarted(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSValue evaluateForDefaultLoader(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void throwCompileOverrideError(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didCreateModuleObject(JSGlobalObject*, ModuleID, JSObject* moduleObject);
JS_EXPORT_PRIVATE std::optional<OptionSet<ModuleObjectTrait>> newModuleObjectTraits(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void adoptModuleObject(JSGlobalObject*, ModuleID, const ModuleObjects&);
JS_EXPORT_PRIVATE void releaseModuleObject(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* moduleObjectWithExportsVariable(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didBindExportsVariable(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willRequireFromUncompiledCode(JSGlobalObject*, ModuleID, JSObject* requirerModuleObject);

JS_EXPORT_PRIVATE void willSetRequireCacheEntry(JSGlobalObject*, ModuleID, JSValue entry);
JS_EXPORT_PRIVATE void willSetBuiltinCacheEntry(JSGlobalObject*, ModuleID, const String& name);
JS_EXPORT_PRIVATE void didDeleteRequireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didDeleteDisplacedModuleEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didDeletePendingMockEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didFailMockImportRoot(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willRunPreloads(JSGlobalObject*);
JS_EXPORT_PRIVATE void didRunPreloads(JSGlobalObject*);
JS_EXPORT_PRIVATE void snapshotEvaluatedModules(JSGlobalObject*);
JS_EXPORT_PRIVATE void retainNewlyEvaluatedModules(JSGlobalObject*);
JS_EXPORT_PRIVATE void didResetModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isLoadInFlight(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isResolvedForInFlightLoads(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void evictFailedModules(JSGlobalObject*, std::optional<ModuleID> patched = std::nullopt);
JS_EXPORT_PRIVATE Vector<String> takeModulesReleasedSincePass(JSGlobalObject*);
JS_EXPORT_PRIVATE bool releaseAbsentModuleImporters(JSGlobalObject*, const String& key, bool onlyImportersFromEarlierTestFiles);
JS_EXPORT_PRIVATE void restoreRetainedModules(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE bool isEvicted(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didCacheSourceInHost(VM&, ModuleID, bool isPluginSource);
JS_EXPORT_PRIVATE bool rejectsReload(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willClearRequireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willReplaceRequireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool tryDelegateModuleToHost(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willReevaluateEntryPoint(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasRequireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasPendingMock(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSObject* requireCacheEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void forEachRequireCacheKey(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE void forEachEvaluatingAsyncModule(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE void resetModulesAfterEvaluation(JSGlobalObject*, const WTF::Function<bool(const String&)>& isKept);
JS_EXPORT_PRIVATE void forEachModuleWithProgramEntry(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE void forEachTLASuspendedModule(JSGlobalObject*, const WTF::Function<void(int64_t turn, const String& key)>&);
JS_EXPORT_PRIVATE void forEachLibraryModuleInRequireCache(JSGlobalObject*, const WTF::Function<void(const String&)>&);
JS_EXPORT_PRIVATE std::optional<ModuleID> findLibraryModuleInRequireCache(JSGlobalObject*, const String& name);
JS_EXPORT_PRIVATE void willChangeRequireOverride(JSGlobalObject*, RequireOverride, bool isOverridden);
JS_EXPORT_PRIVATE void willSetExtensionLoader(JSGlobalObject*, StringView extension);

JS_EXPORT_PRIVATE bool isModuleNamespace(JSCell*);
JS_EXPORT_PRIVATE bool tryReplaceExport(JSCell* moduleNamespace, UniquedStringImpl* name, JSValue);
bool tryReplaceExportOfRecord(JSGlobalObject*, AbstractModuleRecord*, UniquedStringImpl* name, JSValue);
JS_EXPORT_PRIVATE String spyOnExportErrorMessage(JSCell* moduleNamespace, UniquedStringImpl* name, const String& uncompiledCaller);
JS_EXPORT_PRIVATE bool mayBeMocked(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE String mockModuleErrorMessage(JSGlobalObject*, const String& path, const String& uncompiledCaller);
JS_EXPORT_PRIVATE bool hasStartedLoading(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasOnlyBeenLinked(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasImportStartedLoading(JSGlobalObject*, ModuleID moduleOrFacade);
JS_EXPORT_PRIVATE bool wasLoadedByImport(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isMockAwaitedByRefusedRequire(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void forEachEvaluatingModuleOnStack(VM&, CallFrame*, const ScopedLambda<void(ModuleID)>&);
JS_EXPORT_PRIVATE bool isBeingEvaluated(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasDisplacedDuringEvaluation(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasRegisteredByFailedLoad(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasLinkedBeforeTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasLinkedByFailedGraph(JSGlobalObject*, ModuleID, bool byEarlierTestFile = false);
JS_EXPORT_PRIVATE bool wasLinkedBeforeLinkFailure(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasImporterLinkedBeforeFailure(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isLinkedForPendingImporter(JSGlobalObject*, ModuleID, std::optional<ModuleID> asking);
JS_EXPORT_PRIVATE bool isFailedLoadRoot(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasDisplacedAfterFailure(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasLoadedBeforeTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void clearLoadedBeforeTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void markEntryOwnedByTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasFailedESModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isFailedESModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void releaseFailedESModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didRemoveEntryForMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void markFetchOrphaned(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasOrphanedFetch(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didRemoveEntryAfterTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didEvictLibraryEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void markVariantEvictedByMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasVariantEvictedByMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasVariantRegisteredByPreload(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasVariantFetchedByTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void scheduleVariantRestore(JSGlobalObject*, ModuleID restoredVariant, const Vector<ModuleID, 2>& forms);
JS_EXPORT_PRIVATE void didRestoreEntryForPreload(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isEntryOwnedByTestFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE std::optional<ModuleID> findVariantToMock(JSGlobalObject*, StringView path, std::optional<ModuleID> file);
JS_EXPORT_PRIVATE Vector<ModuleID, 2> importAttributeVariants(JSGlobalObject*, StringView path);
enum class FactoryFailurePolicy : bool { CacheError, RetryFactory };
JS_EXPORT_PRIVATE void deferMock(JSGlobalObject*, ModuleID, FactoryFailurePolicy = FactoryFailurePolicy::CacheError, bool isFromPreload = false);
JS_EXPORT_PRIVATE void didFailImportOriginalFactory(JSGlobalObject*, ModuleID, JSValue error);
JS_EXPORT_PRIVATE bool hasMockImportsAwaitingSource(JSGlobalObject*);
JS_EXPORT_PRIVATE bool hasInFlightOriginalRequest(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didFinishTests(JSGlobalObject*);
JS_EXPORT_PRIVATE bool hasDeferredFetchToEvict(JSGlobalObject*);
JS_EXPORT_PRIVATE void willRunTestFileAgain(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void markFactoryAbandoned(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void markMockPassthrough(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void deferMockAfterAsyncFailure(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void retainFactoryLoadedImporters(JSGlobalObject*, ModuleID mocked);
JS_EXPORT_PRIVATE void clearMockLoadErrors(JSGlobalObject*);
JS_EXPORT_PRIVATE void discardDetachedLoadErrors(JSGlobalObject*);
JS_EXPORT_PRIVATE void finishLoadsAwaitingLoader(JSGlobalObject*);
enum class RetainedOriginalPolicy : uint8_t { DiscardWithMock, Keep };
JS_EXPORT_PRIVATE void didRemoveMock(JSGlobalObject*, ModuleID, RetainedOriginalPolicy = RetainedOriginalPolicy::DiscardWithMock, bool wasInstalledOnPreloadMock = true, bool restoresDisplacedModule = true);
JS_EXPORT_PRIVATE void markMockLingering(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isMockLingering(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasDelayedImport(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isRequestedByDetachedLoad(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void retryMockInDetachedLoads(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool needsRetryInDetachedLoad(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE uint64_t factoryCallOfResumedLoad(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void removeUnusedLingeringMocks(JSGlobalObject*);
JS_EXPORT_PRIVATE bool importsStatically(JSGlobalObject*, ModuleID importer, ModuleID imported);
JS_EXPORT_PRIVATE bool isImportedByStatement(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasLoadedByPreloadHook(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void setNeedsMockReinstall(JSGlobalObject*, ModuleID, bool waits = true);
JS_EXPORT_PRIVATE bool evaluateLinkedModuleBeforeMock(JSGlobalObject*, ModuleID, bool displacesEarlierMock, bool displacesModule, bool keepsOriginalIntact);
JS_EXPORT_PRIVATE JSPromise* pendingEvaluation(JSGlobalObject*, ModuleID, bool hasAlreadyWaited);
JS_EXPORT_PRIVATE JSPromise* pendingEvaluationInGraph(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void countMockCall(JSGlobalObject*, std::optional<ModuleID>, ASCIILiteral otherwise, ASCIILiteral call, ASCIILiteral factory);
JS_EXPORT_PRIVATE void evaluateCommonJSModuleBeforeMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool tryEvaluateESModuleBeforeMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool startDetachedOriginalLoad(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool tryStartAsyncOriginal(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isOriginalEvaluatedOutsideRegistry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void restoreOriginalForRequire(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasOriginalEvaluated(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* evaluateOriginalUnderMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isCompiledOriginal(JSGlobalObject*, const String& key);
JS_EXPORT_PRIVATE bool hasLoadedCommonJSFile(JSGlobalObject*);
JS_EXPORT_PRIVATE bool tryEvaluateCommonJSOriginal(JSGlobalObject*, const String& key, const ArgList&);
JS_EXPORT_PRIVATE void willReplaceMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool tryRestoreCompiledOriginal(JSGlobalObject*, AbstractModuleRecord* original);
JS_EXPORT_PRIVATE void restoreOriginalLeftByEarlierMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didRejectRequire(JSGlobalObject*, const String& path);
enum class MockEnd : bool { Removed, Replaced };
JS_EXPORT_PRIVATE void willEndMockOfBuiltin(JSGlobalObject*, ModuleID, MockEnd);
JS_EXPORT_PRIVATE void willImportOriginal(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void noteOriginalRequiredFromSource(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSPromise* pendingFactoryInGraph(JSGlobalObject*, ModuleID, JSValue& state);
JS_EXPORT_PRIVATE bool didMockFailForImportStatement(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willFetchAfterTick(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didResumeFetchAfterFactory(JSGlobalObject*);
JS_EXPORT_PRIVATE bool wasReleasedByLoader(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasInstances(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void tellIfMockedBelowImport(JSGlobalObject*, ModuleID file, std::optional<ModuleID> caller);
JS_EXPORT_PRIVATE bool isReloadableImport(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isReleased(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isMockLoadedInPreload(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasLoadedInPreload(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isActiveMockRegistered(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void registerEvaluatedModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void reportKeptImporters(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void releaseModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void releaseCommonJSFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void setModuleWithSettledFactory(JSGlobalObject*, std::optional<ModuleID>);
JS_EXPORT_PRIVATE bool releaseModuleAndImporters(JSGlobalObject*, ModuleID, std::optional<ModuleID> asking);
JS_EXPORT_PRIVATE void setReplacedTestFile(JSGlobalObject*, std::optional<ModuleID>);
JS_EXPORT_PRIVATE void releaseImporters(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void releaseMockImporters(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void releaseEarlierImporters(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE uint32_t momentOfLoads(JSGlobalObject*);
JS_EXPORT_PRIVATE bool hasBatchedFetches(JSGlobalObject*);
JS_EXPORT_PRIVATE void forgetMockRegisteredAfter(JSGlobalObject*, ModuleID, uint32_t moment);
JS_EXPORT_PRIVATE void releasePatchedModuleOfCycle(JSGlobalObject*, ModuleID, bool isFactoryPending);
JS_EXPORT_PRIVATE void releaseEndedMockImporters(JSGlobalObject*);
JS_EXPORT_PRIVATE void restorePreloadInstances(JSGlobalObject*);
JS_EXPORT_PRIVATE bool willReloadAfterMockRemoval(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void releaseRemovedMocks(JSGlobalObject*);
JS_EXPORT_PRIVATE bool fulfillDeferredFetches(VM&, uint64_t completedDiskReads = UINT64_MAX);
JS_EXPORT_PRIVATE void didDrainDeferredFetchJobs(VM&);
JS_EXPORT_PRIVATE std::optional<uint32_t> firstDeferredBatchNumber(VM&);
JS_EXPORT_PRIVATE bool shouldDeferFetchFulfillment(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void deferFetchFulfillment(JSGlobalObject*, ModuleID, JSPromise* fetched, AbstractModuleRecord*);
JS_EXPORT_PRIVATE void didFetch(JSGlobalObject*, AbstractModuleRecord*);
JS_EXPORT_PRIVATE void noteHostImportInFlight(JSGlobalObject*, ModuleID, JSPromise*, bool isFromOwnFactory);
JS_EXPORT_PRIVATE void noteHostImportCall(JSGlobalObject*, ModuleID, JSPromise*);
JS_EXPORT_PRIVATE bool isMockUnseen(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasOriginalRequiredFirst(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void evictLoadedModules(JSGlobalObject*);
JS_EXPORT_PRIVATE void evictAtNextLoad(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void evictAbandonedModules(JSGlobalObject*);
JS_EXPORT_PRIVATE void didPatchPendingMock(JSGlobalObject*, ModuleID mocked);
JS_EXPORT_PRIVATE bool wasEvictedOnceLoaded(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool isOriginalEvicted(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willEndMocksOfTestFile(JSGlobalObject*);
JS_EXPORT_PRIVATE void didEndMocksOfTestFile(JSGlobalObject*);
JS_EXPORT_PRIVATE void didRequireOwnOriginal(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool takeRequirerOfOwnOriginal(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool tryEvictRequirerOfOriginal(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSValue exportsBeforeMock(JSGlobalObject*, ModuleID, bool evenIfEvaluationFailed, bool* didExportReadThrow = nullptr);
JS_EXPORT_PRIVATE JSValue retainedOriginalExports(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSValue revalidateRetainedOriginalExports(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void retainOriginalExports(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void stashActualKeyExports(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE bool mayTakeRunUnderActualKey(JSGlobalObject*, ModuleID file);
enum class ActualKeyExports : uint8_t { None,
    Restored,
    Superseded };
JS_EXPORT_PRIVATE ActualKeyExports restoreActualKeyExports(JSGlobalObject*, ModuleID file, bool keepsSuperseded);
JS_EXPORT_PRIVATE bool tryEvictUnderActualKey(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* retainedOriginalModuleObject(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void clearRetainedOriginalExports(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE bool isUnderActualKey(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void setIsUnderActualKey(JSGlobalObject*, ModuleID file, bool);
JS_EXPORT_PRIVATE bool wasOriginalRestoredToOwnKey(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE void setWasOriginalRestoredToOwnKey(JSGlobalObject*, ModuleID file, bool);
JS_EXPORT_PRIVATE void setHasNoTestFile(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSValue exportsBeforePatch(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE bool isPatchedByMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasPatchedAfterEvaluation(JSGlobalObject*, ModuleID);
enum class LoadedBy : uint8_t { Nobody, Preload, EarlierTestFile, ThisTestFile };
JS_EXPORT_PRIVATE LoadedBy evaluatedBy(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE LoadedBy fetchedBy(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didRemoveActiveMockEntry(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE LoadedBy activeMockReloadedBy(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasActiveMockEntryRemoved(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSObject* namespaceBeforeMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE JSPromise* pendingOriginalEvaluation(JSGlobalObject*, ModuleID, std::optional<ModuleID> factoryCallerLoad);
JS_EXPORT_PRIVATE void didCallHostImport(JSGlobalObject*, ModuleID imported, JSPromise*);
JS_EXPORT_PRIVATE JSValue mockedModuleEvaluationError(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool runMocksInGraph(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didChangePlugins(JSGlobalObject*);
JS_EXPORT_PRIVATE bool setMock(JSGlobalObject*, ModuleID, JSObject* mock, JSValue requireReturnValue, bool keepsEarlierMockForImporters = false);
JS_EXPORT_PRIVATE bool tryKeepOriginalIntact(JSGlobalObject*, ModuleID, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking, bool evenIfEvaluationFailed = false);
JS_EXPORT_PRIVATE bool hasIntactOriginal(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void displaceEntryParent(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void reportReloadableFileKept(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didRepeatMockOfKeptFile(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void noteKeptOriginal(JSGlobalObject*, ModuleID, bool isAutomock);
JS_EXPORT_PRIVATE void reportMockedFileKept(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void reportIfFileWasReleased(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool ranDuringMockThatOnlyWaited(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool tryReleaseCommonJSFileDroppedByLoader(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool keepCommonJSFileDroppedByLoader(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didDisplaceIntactOriginal(JSGlobalObject*, ModuleID, bool evenIfLoadedEarlier);
JS_EXPORT_PRIVATE bool isDisplacedByMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE AbstractModuleRecord* originalModuleRecord(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didBeginTestFile(JSGlobalObject*);
JS_EXPORT_PRIVATE bool hasActiveMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool hasUnevaluatedActiveMockImporter(JSGlobalObject*, ModuleID, std::optional<ModuleID> asking);
JS_EXPORT_PRIVATE bool tryKeepActiveMock(JSGlobalObject*, ModuleID, bool remainsOriginal, bool keepsCurrentTestFileLoads);
struct ActiveMockExports {
    JSObject* namespaceObject { nullptr };
    JSValue requiredValue;
};
JS_EXPORT_PRIVATE ActiveMockExports retainedActiveMockExports(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasRequiredUnderActiveMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE bool wasRequiredUnderPreloadMock(JSGlobalObject*, ModuleID file);
JS_EXPORT_PRIVATE JSObject* activeMockForNamespace(JSCell* moduleNamespace);
JS_EXPORT_PRIVATE bool hasMockSlot(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void willMockModule(JSGlobalObject*, ModuleID, JSValue requireReturnValue, bool hasCommonJSExports, bool hasFactory, JSObject* mock);
JS_EXPORT_PRIVATE void didMockModule(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void didStopInstallingMock(JSGlobalObject*, ModuleID);
JS_EXPORT_PRIVATE void mockExport(JSGlobalObject*, ModuleID, UniquedStringImpl* name, JSValue);
JS_EXPORT_PRIVATE bool hasESModuleMarker(JSCell* moduleNamespace);
JS_EXPORT_PRIVATE void setESModuleMarker(JSCell* moduleNamespace, bool);

JS_EXPORT_PRIVATE void setVerbose(bool);

struct AddressSpaceToStart {
    size_t needed { 0 };
    size_t left { 0 };
    size_t largestStepOfAllocator { std::numeric_limits<size_t>::max() };
    bool isRefused { false };
};
JS_EXPORT_PRIVATE AddressSpaceToStart askForAddressSpaceToStart(size_t sizeOfProgram, std::span<const size_t> stepsOfAllocatorFromLargest);

struct PagesForNewThread {
    void* pages { nullptr };
    bool isRefused { false };
};
JS_EXPORT_PRIVATE PagesForNewThread tryReservePagesForNewThread(bool threadsAreEnding);
JS_EXPORT_PRIVATE PagesForNewThread tryReservePagesForFirstThread();
JS_EXPORT_PRIVATE void releasePagesForNewThread(void* pages);
JS_EXPORT_PRIVATE void adoptPagesForNewThread(void* pages);
JS_EXPORT_PRIVATE void releaseUnusedPagesOfThread();
JS_EXPORT_PRIVATE void didStartThread(VM*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
