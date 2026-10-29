/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleHost.h"
#include "AOTModuleRecord.h"
#include "AOTModuleTable.h"
#include "AOTModuleWords.h"
#include "JSPromise.h"
#include "MicrotaskQueue.h"
#include "ModuleMap.h"
#include "RememberedSlot.h"
#include "ScriptFetcher.h"
#include "SourceOrigin.h"
#include "Strong.h"
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/RefPtr.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class JSCellButterfly;
class JSModuleLoader;
class ModuleLoadingContext;
class ScriptFetchParameters;
class ThrowScope;

namespace AOT {

class ModuleLinkWalk;
struct EvaluationInProgress;
struct Instance;
struct State;

enum class ModuleStatus : uint8_t {
    Unlinked = static_cast<uint8_t>(CyclicModuleRecord::Status::Unlinked),
    Linking,
    Linked,
    Evaluating,
    EvaluatingAsync,
    Evaluated,
    Failed,
};
static_assert(static_cast<uint8_t>(ModuleStatus::Evaluated) == static_cast<uint8_t>(CyclicModuleRecord::Status::Evaluated));

enum class SynchronousEvaluationReason : uint8_t { Require, DeferredNamespace };
// While require() loads a graph the jobs of the program wait for its return in any case, and the steps of the loader are in a queue of their own.
enum class JobsOfImportedFiles : uint8_t { StayQueued, WaitForLoader, StepsOfLoaderWaitForLoader };
enum class ErrorOfImportedFile : uint8_t { IsThrown, IsLeftToLoader };

struct LinkContext {
    ModuleLinkingStack& stack;
    unsigned index { 0 };
    RefPtr<ScriptFetcher> scriptFetcher { };
};

// capturedFields: in place of capturedExports, until somebody asks for all the names.
#define FOR_EACH_REFERENCE_OF_LOADED_MODULE(macro) \
    macro(JSObject*, namespaceObject) \
    macro(JSObject*, deferredNamespaceObject) \
    macro(JSObject*, importMeta) \
    macro(JSObject*, requirer) \
    macro(JSObject*, objectMadeByRequire) \
    macro(JSValue, requireFunction) \
    macro(JSValue, requiredValue) \
    macro(JSObject*, exportsWhileRequired) \
    macro(JSObject*, capturedExports) \
    macro(JSObject*, deferredExportsSource) \
    macro(JSCellButterfly*, capturedFields) \
    macro(JSCellButterfly*, capturedStars) \
    macro(JSObject*, sourceOfCapturedFields) \
    macro(AbstractModuleRecord*, uncompiledRecord)

struct EvaluationContext {
    ModuleEvaluationStack& stack;
    unsigned index { 0 };
    std::optional<int64_t> referrerAsyncOrder { };
    JSPromise* importPromise { nullptr };
};

class Modules {
    WTF_MAKE_NONCOPYABLE(Modules);
    WTF_MAKE_TZONE_ALLOCATED(Modules);
public:
    using ModulePhase = AbstractModuleRecord::ModulePhase;

    explicit Modules(State&);
    ~Modules();

    Instance& instance() { return m_instance; }
    const ModuleTable& table() const { return m_table; }
    // Moves on when the entry point is linked and when the exports of a file are captured again: a walk made before may pass over what has work now.
    unsigned linkEpoch() const { return m_linkEpoch; }
    // No link has anything left to do with the module.
    bool isSettledForLinks(ModuleID module) const { return (m_modulesSettledForLinks[module.index() / 64] >> (module.index() % 64) & 1) || checkIsSettledForLinks(module); }
    // A bit for each module that was found to be settled.
    std::span<const uint64_t> modulesSettledForLinks() const { return m_modulesSettledForLinks.span(); }
    const ModuleData& data(ModuleID module) const { return m_table.data(module); }
    const ModuleHost& host() const;
    Identifier identifier(uint32_t number) const;
    ModuleStatus status(ModuleID) const;
    ModuleRecord* recordIfExists(ModuleID) const;
    ModuleRecord* ensureRecord(ModuleID);
    JSValue value(ModuleID) const;
    JSValue readExport(ModuleID, unsigned index);
    JSValue readExportIfInitialized(ModuleID, unsigned index) const;
    bool isExportAmbiguous(ModuleID, unsigned index) const;
    JSObject* capturedExports(ModuleID);
    // Empty if the module has no export of the name.
    JSValue capturedValue(ModuleID, const Identifier&);
    bool hasCapturedExport(ModuleID, const Identifier&);
    // Hands over no value for a deferred export.
    void forEachCapturedExport(ModuleID, const ScopedLambda<void(const Identifier&, JSValue)>&);
    // What a table of exports holds under the name. In the captured exports of a module the table itself stands for a deferred export: capturedValue() reads that one.
    static JSValue storedValue(JSGlobalObject*, JSObject* table, const Identifier& name);
    // As AbstractModuleRecord::getModuleNamespace(): the caller that creates the object decides whether it is extensible.
    JSObject* moduleNamespace(ModuleID, ModulePhase, bool shouldPreventExtensions = true);
    JSObject* importMeta(ModuleID);
    JSCell* builtinCell(BuiltinID, ModuleID);
    JSString* pathString(ModuleID);
    JSString* resolve(ModuleID, ResolvedSpecifierForm);
    const SourceOrigin& sourceOrigin(ModuleID);
    void throwFailure(ModuleFailureID, bool isForRequire);
    void throwExportNotFound(const RequiredNameData&);
    void throwExportAmbiguous(const RequiredNameData&);
    void didRequestFromHost(ModuleID module) { m_firstModuleRequestedByHost = m_firstModuleRequestedByHost.value_or(module); }
    std::optional<ModuleID> firstModuleRequestedByHost() const { return m_firstModuleRequestedByHost; }
    // `bun run` loads the entry point as the one import of the module bun:main: the graph is loaded one step of the loader behind the entry point. Says so once.
    bool takeStepOfMainModule(ModuleID module) { return m_firstModuleRequestedByHost == module && !std::exchange(m_didTakeStepOfMainModule, true); }
    // An import() that loads the entry point ahead of the program loads it as any other module, and bun:main finds it loaded.
    void willBeLoadedByImport(ModuleID module) { m_didTakeStepOfMainModule |= m_firstModuleRequestedByHost == module; }
    // Ahead of that step bun:main waits for one promise and whoever else asks for the entry point for another. Says so once.
    bool takeLoadPromiseBelowMainModule(ModuleID module) { return m_firstModuleRequestedByHost == module && !m_didTakeStepOfMainModule && !std::exchange(m_didTakeLoadPromiseBelowMainModule, true); }
    // The body of bun:main runs where the body of any importer does: behind the last statement of the entry point, or in the job that ends its evaluation.
    JSPromise* evaluateMainModule(ModuleID entryPoint, JSPromise* evaluation);
    bool mainModuleWaitsForItsImport() const { return !!m_capabilityOfMainModule; }
    void runMainModuleThatWaited();
    void failMainModuleThatWaited();
    bool refusesCodeOutsideImage() const { return !m_admitsCodeOutsideImage; }
    // The text of a data: URL that the program imports under a constant name is part of the program.
    bool isTextNamedByProgram(StringView url) const;
    bool refusesLoadsByName() const { return !m_loadsModulesByName && !m_isAskingHost && !m_runsUncompiledCode; }
    bool isAskingHost() const { return m_isAskingHost; }
    // A name that the build did not follow finds an entry point and no other file of the image with code.
    bool refusesRequestFromHost(ModuleID module) const { return !m_admitsInspector && !m_isAskingHost && !data(module).isEntryPoint() && (data(module).kind != ModuleKind::Data || !m_findsDataFilesByName) && m_table.hasFilePath(module) && !data(module).isLibraryCodeWithFilePath(); }
    void admitCodeNamedBy(JSValue specifier);
    void abandonEvaluation(ModuleID, JSValue error);
    template<typename Visitor> void visitRoots(Visitor&);

    bool loadRuntimeModules(ModuleID root);
    void link(ModuleID root);
    void captureSyntheticExports(ModuleID root, VisitedModuleRecords&);
    void finishLink(ModuleID root, LinkContext&);

    JSPromise* evaluate(ModuleID root, std::optional<int64_t> referrerAsyncOrder, JSPromise* importPromise, const ModuleLinkWalk* = nullptr, bool isLinked = false);
    void evaluateSynchronously(ModuleID, SynchronousEvaluationReason);
    void innerModuleEvaluation(ModuleID root, EvaluationContext&);
    void gatherAsynchronousTransitiveDependencies(ModuleID, OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen);
    bool readyForSyncExecution(ModuleID, VisitedModuleRecords& seen);
    // The modules that compiled code begins to evaluate below the body fail with it.
    JSValue callBodyOfModuleEnteredByLoader(ModuleID module) { return callBodyUnderOwnMark(module, false); }
    void evaluateModuleInRecord(ModuleID);
    // Says whether the start that asks goes on to run the module.
    bool beginModuleInRecord(ModuleID);
    // Says whether a file ran.
    bool evaluateImportedCommonJSFiles(ModuleID root, JobsOfImportedFiles = JobsOfImportedFiles::StayQueued, ErrorOfImportedFile = ErrorOfImportedFile::IsThrown, ModuleGraphLoadingState* = nullptr);
    void queueJobsThatWaitForLoader();
    void handGraphsToLoader();
    void didLoadGraph(ModuleID root);
    void didFailToLoadGraph(ModuleID root);
    void evaluateFilesOfLevel(ModuleID moduleOnWay, bool runsWhenLoaded);
    void captureExportsAgain(ModuleID);
    // The start of the module is on the stack only to run the files that the module imports.
    bool isRunningFilesOf(ModuleID root) const { return m_rootWhoseFilesRun == root; }
    bool stopLoadingBehindFile(ModuleID file);
    void evaluateLoadedCommonJSFile(ModuleID);
    bool isCommonJSFileOrFace(ModuleID module) const { return data(module).kind == ModuleKind::CommonJs || m_table.fileBehindFace(module); }
    // The loader can load a file of the program as it loads code that is not compiled.
    bool isFileOfProgram(ModuleID module) const { return !data(module).isOfLibrary() && (data(module).kind == ModuleKind::SourceText || data(module).kind == ModuleKind::CommonJs); }
    bool isLoadedWithRequests(ModuleID module) const { return isFileOfProgram(module) && data(module).kind == ModuleKind::SourceText; }
    // `bun run` takes the loader's whole round for a data file too.
    bool isImportedByLoader(ModuleID module) const { return isFileOfProgram(m_table.fileBehindFace(module).value_or(module)) || (data(module).kind == ModuleKind::Data && !data(module).isOfLibrary()); }
    bool isBuiltinModuleOfLoader(ModuleID) const;
    bool isBuiltinModuleNewToLoader(ModuleID) const;
    bool needsLoaderForRequire(ModuleID);
    bool canTellHowGraphIsLoaded(ModuleID root);
    void appendRequestsForLoader(ModuleID, ModuleRecord*, ModuleGraphLoadingState&);
    ModuleGraphLoadingState* loadThatWaitsFor(ModuleID requester, const Identifier& key, ModuleGraphLoadingState* loadOfRequester, unsigned& jobsUntilLoadHearsOfFailure);
    ModuleRecord* appendRequestsOfUncompiledCode(ModuleID, ModuleRecord*);
    void collectKeysOfUncompiledCode(ModuleID, OrderedHashSet<UniquedStringImpl*>& keys);
    bool comesToCodeThatDoesNotLoad(ModuleID root);
    bool setIsWalkingRequests(bool isWalkingRequests) { return std::exchange(m_isWalkingRequests, isWalkingRequests); }
    // A graph is being loaded until its root is linked.
    unsigned numberOfGraphsBeingLoaded();
    bool comesToLoadInFlight(ModuleID required);
    bool hasNotBegunAsFile(ModuleID) const;
    void markModulesBelowWalkedRoot(ModuleID root);
    void didStartLoad(JSPromise*, ModuleID root);
    enum class LoadOfFile : uint8_t { IsAskedFor, HasComeToFile, IsRootOfImport };
    ModuleRecord* recordOfCommonJSFile(ModuleID, LoadOfFile = LoadOfFile::HasComeToFile);
    void noteLoadOfFile(ModuleID fileOrFace, LoadOfFile = LoadOfFile::HasComeToFile);
    bool asyncLoadMustWaitFor(ModuleID fileOrFace) const;
    bool importsFileBeingEvaluated(ModuleID root) const;
    std::optional<ModuleID> fileToWaitFor(const Identifier& key);
    JSPromise* deferRequest(ModuleID file, ModuleLoadingContext*, bool useImportMap);
    bool hasDeferredLoads() const { return !m_deferredRequests.isEmpty() || !m_deferredImports.isEmpty(); }
    void didRequestModule(const Identifier& key, JSCell* payload);
    void didSettleLoad(const ModuleGraphLoadingState&);
    void didFailToMakeModule(const Identifier& key);
    bool isNotLoaded(ModuleID);
    // Of an ES module of the program that has neither a record nor a status.
    bool isNotLoadedWithoutStatus(ModuleID, const ModuleData&);
    // An ES module of the program whose status stands in the table and says that it is linked, at the least.
    bool isLoadedModuleWithoutRecord(ModuleID) const;
    bool wasLoadedWithEarlierGraph(ModuleID);
    bool wasLoadedWithGraph(ModuleID) const;
    bool setIsAskingHost(bool isAskingHost) { return std::exchange(m_isAskingHost, isAskingHost); }
    bool wasImportedWhileItRan(ModuleID file) const;
    void dropErrorOfFile(ModuleID fileOrFace);
    void evaluateCommonJSForLoader(ModuleID file);
    JSValue execute(ModuleID);

    // A whole require(): who required whom is recorded here, whoever asks. Without a referrer it is the evaluation and its answer alone: for the host's own loads, and for a site with operations around it that record.
    JSValue require(ModuleID, ModuleID referrer = { });
    void evaluateCommonJS(ModuleID);
    JSValue commonJSExports(ModuleID) const;
    JSValue exportsSeenByLoader(ModuleID);
    void setCommonJSExports(ModuleID module, JSValue exports) { setValue(module, exports); }
    void recordRequire(ModuleID, ModuleID referrer);
    JSObject* moduleObject(ModuleID);
    JSObject* mainModuleObject();
    JSObject* parentModuleObject(ModuleID);
    bool forEachNewChild(ModuleID, const ScopedLambda<void(JSObject*)>&);
    JSValue requireSpecifier(JSValue specifier, ModuleID referrer, bool isNamedByBuild);
    JSValue resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm);

    JSPromise* import(ModuleID, ModuleID referrer, JSValue options = jsUndefined());
    // The host hands what it gets to import() in code that is not compiled, which gives the program a promise of its own.
    enum class ImportCaller : bool { CompiledCode, Host };
    JSPromise* startImport(ModuleID, std::optional<int64_t> referrerAsyncOrder, ImportCaller = ImportCaller::CompiledCode);
    JSPromise* importDynamic(JSValue specifier, JSValue options, ModuleID referrer);
    void continueImport(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    void resolveWithNamespace(JSPromise* importPromise, ModuleID);
    void finishImportOfEvaluatedModule(JSPromise* importPromise, ModuleID, JSValue error);
    // Says whether the jobs of that import() are counted.
    bool continueCountedImport(JSPromise* importPromise, JSValue module, JSValue referrerAsyncOrder);
    void handCountedImportsToLoader();
    bool comesToCountedImport(ModuleID required);

    enum class RecordHolder : uint8_t { Anyone, Loader };
    ModuleRecord* exposeRecord(ModuleID, RecordHolder = RecordHolder::Anyone);
    void retireRecordsOfSettledLoads();
    AbstractModuleRecord* uncompiledRecord(ModuleID);
    // The record that stands for the module in ResolveExport() and GetExportedNames().
    AbstractModuleRecord* recordForResolution(ModuleID);
    JSObject* uncompiledNamespace(ModuleID, ModulePhase);

private:
    friend class RequestToHostScope;

    // The roots are visited at every stop of the mutator, and the loaded modules only after a store.
    template<typename T>
    class Reference {
        WTF_MAKE_NONCOPYABLE(Reference);
    public:
        Reference() = default;

        operator T() const { return m_value; }
        explicit operator bool() const { return !!m_value; }
        T operator->() const { return m_value; }
        T get() const { return m_value; }

        void set(Modules& modules, T value)
        {
            m_value = value;
            modules.m_storedReferences.recordAll();
        }

    private:
        T m_value { };
    };

    struct LoadedModule {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LoadedModule);

        Reference<JSObject*>& namespaceObjectFor(ModulePhase phase) { return phase == ModulePhase::Defer ? deferredNamespaceObject : namespaceObject; }

#define AOT_DECLARE_REFERENCE(type, name) Reference<type> name;
        FOR_EACH_REFERENCE_OF_LOADED_MODULE(AOT_DECLARE_REFERENCE)
#undef AOT_DECLARE_REFERENCE
        bool isBeingRequired { false };
        bool capturedFieldsAreOfESModule { false };
        bool capturedFieldsAreOfRuntimeModule { false };
        Identifier resolvedKey;
        bool wasLoadedWhileItRan { false };
        bool isAnsweredWithNamespace { false };
        bool bindsEarlierCapture { false };
        bool didDropError { false };
        bool isBeingLoadedByLoader { false };
        BitVector ambiguousExports;
        std::unique_ptr<SourceOrigin> sourceOrigin;
        Vector<ModuleID> requiredModules;
        unsigned numberOfChildrenShown { 0 };
        std::optional<ModuleID> firstRequirer;
        // Of a CommonJS file whose body the run time called: m_synchronousLoadCount at that call.
        std::optional<uint32_t> synchronousLoadCountAtStart;
    };

    struct DynamicExport {
        Identifier name;
        JSValue capturedValue;
        JSObject* liveSource { nullptr };
    };

    StoredModuleStatus storedStatus(ModuleID) const;
    void setStoredStatus(ModuleID, StoredModuleStatus);
    ModuleStatus statusInRecord(ModuleID) const;
    void setStatus(ModuleID, ModuleStatus);
    void setFailed(ModuleID, JSValue error);
    ModuleRecord* ensureRecordWithoutStatus(ModuleID);
    void retireRecord(ModuleRecord*);
    bool hasRunToItsEndWithItsCycle(const ModuleRecord&) const;
    void setValue(ModuleID, JSValue);
    LoadedModule& ensureLoaded(ModuleID);
    LoadedModule* loadedIfExists(ModuleID) const;
    void throwEvaluationError(ModuleID);
    static JSPromise* rejectedPromiseWithPendingException(JSGlobalObject*, ThrowScope&);
    Identifier moduleKey(ModuleID);
    Identifier keyOfRequest(ModuleID target);
    JSObject* namespaceIfCreated(ModuleID, ModulePhase) const;
    void setNamespace(ModuleID, ModulePhase, JSObject*);
    JSObject* createNamespace(ModuleID, ModulePhase, bool shouldPreventExtensions);
    JSObject* createTableOfExports();
    JSModuleLoader* loader() const;

    enum class NativeModuleLoading : bool { Eager, Deferred };
    bool loadRuntimeModules(const ModuleLinkWalk&, NativeModuleLoading = NativeModuleLoading::Eager);
    void link(const ModuleLinkWalk&);
    void captureSyntheticExports(const ModuleLinkWalk&, VisitedModuleRecords&);
    void finishLink(const ModuleLinkWalk&, LinkContext&);
    void didLinkEntryPoint();
    bool didCaptureExports(ModuleID) const;
    void captureExports(ModuleID);
    void copyExportsOfFinishedFile(ModuleID face);
    bool isNativeModule(ModuleID) const;
    JSObject* generateNativeModuleExports(ModuleID, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values);
    JSValue requireNativeModule(ModuleID);
    void generateNativeModuleForLoader(ModuleID);
    void bindLatestCapture(ModuleID);
    void captureExportsBehindEvaluation(ModuleID);
    void captureExportsOfFilesBeingEvaluated(ModuleID root);
    JSValue evaluateForCapture(ModuleID);
    bool tryCaptureFields(ModuleID, JSValue exports);
    JSValue capturedField(const LoadedModule&, UniquedStringImpl* name) const;
    void makeTableOfCapturedFields(LoadedModule&);
    void fillCapturedExports(ModuleID);
    void fillDeferredExports(ModuleID importer);
    AbstractModuleRecord::Resolution::Type resolveRequiredName(ModuleID exporter, const Identifier&);
    // Throws for the first name that an unlinked module of the list requires and that its exporter lacks or has from two bindings.
    void checkRequiredNames(std::span<const ModuleID> modules);
    bool isDeferredExport(const LoadedModule&, const Identifier&) const;
    bool findsItsNamesInLibraryAlone(ModuleID) const;
    Vector<DynamicExport> collectDynamicExports(ModuleID);

    void evaluateWithOwnStack(ModuleID root);
    size_t numberOfJobsThatWouldWait(JobsOfImportedFiles) const;
    void holdJobsBehind(size_t numberOfJobs, ModuleID file, JobsOfImportedFiles = JobsOfImportedFiles::WaitForLoader);
    bool hasQueuedTicks() const { return host().hasQueuedTicks && host().hasQueuedTicks(m_instance.globalObject); }
    struct LevelOfLoad;
    struct PlanOfLoad;
    bool planLevelsOfLoad(ModuleID root, RefPtr<PlanOfLoad>* planWithoutWay = nullptr);
    bool isLeftAsLoaded(ModuleID) const;
    bool isModuleWithoutRecord(const ModuleData&, StoredModuleStatus) const;
    bool isNeitherOfProgramNorData(const ModuleData&, StoredModuleStatus) const;
    bool isFoundWithoutRecord(ModuleID, const ModuleData&, StoredModuleStatus);
    bool isStillToBeLoaded(ModuleID);
    void layWayOfLoad(PlanOfLoad&, LevelOfLoad* above);
    void layWayOfLoadAgain(LevelOfLoad&);
    void countStepsOfLevel(PlanOfLoad&, size_t depth);
    uint32_t countStepsOfWholeGraph(const PlanOfLoad&, std::span<uint32_t> firstRequestOfPlace, std::span<uint32_t> allRequestedPlaces);
    uint32_t takeMarkOfSearch();
    void takeUpWaysOfLoads();
    void markModulesOfPlan(const PlanOfLoad&);
    JSValue callBody(ModuleID);
    JSValue callBodyUnderOwnMark(ModuleID, bool bodyStoresStatus, bool runsImportedFilesAlone = false);
    Vector<ModuleID, 8> modulesThatEvaluationWillBegin(ModuleID root);
    void evaluateByCallingBody(ModuleID module) { callBodyUnderOwnMark(module, true); }
    void didThrowInBody(ModuleID, JSValue error);
    void failEvaluatedModulesOfUnfinishedCycles(std::span<const ModuleID> failedModules, JSValue error);
    void recordEvaluationIfNothingIsLeftToRun(ModuleID);
    bool isPassedOverByEvaluationInProgress(ModuleID) const;

    std::optional<ModuleID> mainCommonJSModule();
    void decideMainCommonJSModule(std::optional<ModuleID>);
    std::optional<ModuleID> firstFileThatLoaderFindsBelow(ModuleID root);
    void lookForMainCommonJSModule();
    void noteLoadOfGraph(ModuleID root);
    JSObject* moduleObjectMadeByRequire(ModuleID);
    bool mayHandOutModuleObject(ModuleID module) const { return data(module).kind != ModuleKind::CommonJs || data(module).hasValue(); }
    JSValue answerRequire(ModuleID);
    JSValue readExportForRequire(JSObject* namespaceObject, const Identifier& name);
    void willRequireESModule(ModuleID);
    void markNamespaceAsRequired(ModuleID);

    JSPromise* importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&&);
    std::optional<ModuleID> findByName(const Identifier& key);
    std::optional<ModuleID> findWithAttributes(const Identifier& key, const ScriptFetchParameters*);
    Identifier keyWithoutForm(ModuleID file);
    std::optional<ModuleID> findFormThatStatementLoaded(const Identifier& key, const ScriptFetchParameters&, std::optional<ModuleID> asked) const;
    void noteFormAnsweredFirst(const Identifier& key, const ScriptFetchParameters&, ModuleID file);
    bool loaderOnlyHandsOverNamespace(ModuleID) const;
    JSPromise* askLoaderForImport(ModuleID, std::optional<int64_t> referrerAsyncOrder);
    bool isAnythingBeingLoaded() const;
    bool canCountJobsOfImport(ModuleID root);
    bool hasImportedFileLeftToRun(ModuleID root) const;
    std::optional<int64_t> asyncEvaluationOrder(ModuleID) const;
    ModuleRecord* registerWithLoader(ModuleID, RecordHolder = RecordHolder::Anyone);
    const Identifier& resolvedKey(ModuleID);

    State& m_state;
    Instance& m_instance;
    ModuleTable m_table;
    std::span<uint8_t> m_storedStatuses;
    UncheckedKeyHashMap<ModuleID, ModuleRecord*> m_records;
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LoadedModule>> m_loaded;
    struct FormAnsweredFirst {
        Identifier name;
        ModuleID module;
    };
    ModuleMap<FormAnsweredFirst> m_formsAnsweredFirst;
    // Whoever asks for one asks for it several times in a row. None is ever removed.
    mutable LoadedModule* m_loadedFoundLast { nullptr };
    mutable ModuleID m_moduleOfLoadedFoundLast;
    RememberedSlotSet m_storedReferences;
    std::unique_ptr<ModuleLinkWalk> takeLinkWalk(ModuleID root);
    void keepLinkWalk(std::unique_ptr<ModuleLinkWalk>&&);
    Vector<std::unique_ptr<ModuleLinkWalk>, 2> m_linkWalksOfLoads;
    bool checkIsSettledForLinks(ModuleID) const;
    unsigned m_linkEpoch { 0 };
    mutable Vector<uint64_t> m_modulesSettledForLinks;
    // Compared, never followed: its job holds the promise.
    JSPromise* m_importThatWaitsForQueuedJobs { nullptr };
    const EvaluationInProgress* m_innermostEvaluation { nullptr };
    // Each load of evaluateSynchronously() has a number. The innermost one that has not returned is the active one.
    uint32_t m_synchronousLoadCount { 0 };
    uint32_t m_activeSynchronousLoad { 0 };
    Structure* m_namespaceStructure { nullptr };
    Structure* m_extensibleNamespaceStructure { nullptr };
    Identifier m_moduleExportsName;
    // The pairs of a requiring and a required module that recordRequire() has seen.
    enum class RecordedRequire : uint8_t { Begun, Returned, BegunAgain, BegunAndMadeObjectOfESModule, ReturnedAndNotListed };
    UncheckedKeyHashMap<uint64_t, RecordedRequire> m_recordedRequires;
    std::optional<ModuleID> m_mainCommonJSModule;
    bool m_isMainCommonJSModuleDecided { false };
    bool m_didSearchBelowEntryPoint { false };
    // While no file has the id "." and no require() of a file has returned.
    Vector<ModuleID, 4> m_modulesFirstRequiredByCallsInProgress;
    // The first CommonJS file below each root that import() was called for in that time.
    Vector<ModuleID, 2> m_firstFilesOfLoads;
    BitVector m_modulesSearchedForFirstFile;
    // `bun run` makes the module object of a file for whoever asks for the file first, and only a require() leaves a parent in it.
    BitVector m_filesRequestedByLoader;
    struct FilesBeingRun {
        ModuleID root;
        // Without one, the start of the root calls them.
        std::optional<ModuleID> fileOrFace;
        FilesBeingRun* outer;
        // The loader of `bun run` has asked for every file of the root by now, in jobs that stand in front of whatever the files queue.
        bool isAheadOfJobsOfLoader { false };
        // In which the loader asks the root for its requests. Without one, the jobs of an import() are counted.
        ModuleGraphLoadingState* load { nullptr };
        size_t numberOfJobsInFrontOfFile { 0 };
        // The loader has gone through the graph as far as the step in which this file runs.
        std::optional<ModuleID> fileHandedToLoader { };
        // By a CommonJS file: how many modules stand between the root and the nearest one that imports it. Made when something asks.
        Vector<uint32_t> distanceOfFiles { };
    };
    void handGraphToLoader(FilesBeingRun&, std::optional<ModuleID> finishedFile = std::nullopt);
    static void runJobsThatAreDue(VM&, MicrotaskQueue&);
    void runStepsOfLoaderInFrontOf(ModuleID file);
    void finishStepInWhichFileRan(ModuleID fileOrFace, JSValue error);
    std::optional<ModuleID> m_fileBeingHandedToLoader;
    FilesBeingRun* m_innermostFilesBeingRun { nullptr };
    void noteRequestByLoader(ModuleID fileOrFace);
    bool wasRequestedByLoader(ModuleID file);
    bool hasReachedLevelOf(FilesBeingRun&, ModuleID file);
    void findDistanceOfFiles(FilesBeingRun&);
    BitVector m_modulesWithMarkOfRequire;
    BitVector m_modulesWithExtensibleNamespace;
    BitVector m_modulesWithEarlierNamespaceInValue;
    BitVector m_modulesEvaluatedUnderAnotherEvaluation;
    std::optional<ModuleID> m_firstModuleRequestedByHost;
    bool m_didTakeStepOfMainModule { false };
    bool m_didTakeLoadPromiseBelowMainModule { false };
    ModuleID m_importOfMainModule;
    Strong<JSPromise> m_evaluationOfImportOfMainModule;
    Strong<JSPromise> m_capabilityOfMainModule;
    bool m_admitsCodeOutsideImage;
    bool m_runsUncompiledCode { false };
    const bool m_admitsInspector;
    const bool m_showsWaysBetweenModuleObjects;
    bool m_loadsModulesByName;
    const bool m_findsDataFilesByName;
    bool m_isAskingHost { false };
    bool m_hasAskedEveryClass { false };
    bool m_isWalkingRequests { false };
    struct LoadInFlight {
        Strong<JSPromise> promise;
        ModuleID root;
        bool isWalked { false };
        bool hasMarksBelowRoot { false };
        bool isLoaded { false };
        RefPtr<PlanOfLoad> planWithoutMarks;
    };
    Vector<LoadInFlight, 2> m_loadsInFlight;
    Vector<ModuleID, 4> m_modulesWhoseRecordsLoaderHolds;
    BitVector m_modulesBelowWalkedRoots;
    // `bun run` loads a graph level by level, and the steps of one level follow each other with no other job between them. The steps for an ES module run no code of the program, so of the ES modules
    // the loader need only go down one way to the lowest level. With the module of a level on that way it asks for every CommonJS file of the level that is left to run, in their order:
    // while a file runs the loader has it in its hands as under `bun run`, for whatever the file or another graph asks of the loader, and a file that throws fails alone.
    // What lay below the root and was not loaded when the loader came to the root. A require() may load a part of it before the loader comes there: what is left stands on the level where it stood.
    struct PlanOfLoad : RefCounted<PlanOfLoad> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlanOfLoad);

        struct Found {
            ModuleID module;
            // A place in the list.
            uint32_t importer;
            // A request for what another module of the same level has asked for hears of it one step later. The most such requests on a way from the root.
            uint32_t laterRequests : 29;
            uint32_t isFile : 1;
            uint32_t wasLoadedSince : 1;
            // Until the next require() comes between.
            uint32_t isKnownToBeLeft : 1;
        };
        struct Level {
            uint32_t first;
            // The most of a module or file of the level, and where one with so many stands.
            uint32_t laterRequests;
            uint32_t placeOfMostLaterRequests;
            bool hasFiles;
        };
        static constexpr uint32_t jobsOfLevel = 5;

        // The levels stand one behind the other, each in the order in which the loader asks.
        Vector<Found, 64> found;
        Vector<Level, 32> levels;
        Vector<uint32_t> knownToBeLeft;
        // From the step in which the loader asks the root for its requests to the one in which the last request so far is heard of.
        uint32_t steps { 0 };
        // The same for the whole graph, where a module asks for one of its own level or of a level above: the levels alone do not say then who hears of what in which step. Zero: they do.
        uint32_t stepsOfWholeGraph { 0 };
        bool asksAcrossLevels { false };
        // Nothing of it was loaded since.
        bool isWhole { true };
        // Of the root.
        Strong<JSCell> load;

        std::span<Found> level(size_t depth) { return found.mutableSpan().subspan(levels[depth].first, (depth + 1 == levels.size() ? found.size() : levels[depth + 1].first) - levels[depth].first); }
    };
    bool isLeftToLoad(PlanOfLoad&, PlanOfLoad::Found&);
    struct CountedImport {
        Strong<JSPromise> promise;
        ModuleID module;
        std::optional<int64_t> referrerAsyncOrder;
        uint32_t serial;
        // Of the jobs in which the loader would load the graph.
        uint32_t jobsRun { 0 };
        // Known from the job in which the loader would ask the root for its requests.
        uint32_t jobOfEvaluation { 0 };
        uint32_t jobOfNextNativeModule { 0 };
        RefPtr<PlanOfLoad> plan { };
        // The loader has the graph in its hands.
        RefPtr<MicrotaskQueue> jobsOfLoader { };
        CountedImport* outerInOwnJob { nullptr };
        // All the jobs in which the loader would load the graph have passed.
        bool isLoaded { false };
    };
    void decideHowCountedImportGoesOn(CountedImport&);
    uint32_t findJobOfNextNativeModule(PlanOfLoad&, uint32_t currentJob) const;
    void forgetCountedImport(const CountedImport&);
    Vector<CountedImport, 2> m_countedImports;
    // Out of the list while their own jobs run.
    CountedImport* m_innermostCountedImportInOwnJob { nullptr };
    void forEachCountedImport(const Invocable<void(CountedImport&)> auto&);
    void noteOrderAndModulesOfOnlyCountedImport();
    // That the loader does not have in its hands.
    uint32_t m_numberOfCountedImports { 0 };
    uint32_t m_serialOfNextCountedImport { 0 };
    // A serial for the call of import() and for each job counted since, of those that are still counted. Kept while there are several.
    Vector<uint32_t, 32> m_orderOfCountedJobs;
    // Their roots, and what their plans have found. Kept while there are several.
    BitVector m_modulesOfCountedImports;
    std::optional<ModuleID> m_rootKnownToBeWatched;
    // What the search below that root found when its jobs were counted. Nothing was loaded since.
    PlanOfLoad* m_planOfRootKnownToBeWatched { nullptr };
    std::optional<ModuleID> m_rootWhoseGraphIsLoaded;
    struct LevelOfLoad {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LevelOfLoad);

        struct FileOfNextLevel {
            ModuleID module;
            // The load of any other importer is not in the hands of the loader: the load of the root waits for the file.
            bool isImportedByModuleOnWay;
        };
        std::optional<ModuleID> next;
        Vector<FileOfNextLevel, 4> filesInFrontOfNext;
        Vector<FileOfNextLevel, 4> filesBehindNext;
        // Of this level, with no code of the program to run.
        Vector<ModuleID, 4> filesWithoutCode;
        RefPtr<PlanOfLoad> plan;
        uint32_t depth { 0 };
        // Of the module on the way, in the plan.
        uint32_t place { 0 };
        bool isAskedFor { false };
    };
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LevelOfLoad>> m_levelsOfLoads;
    // From where a module on a way lists its requests to where the loader asks for them, in the same step.
    struct FileAskedForOnBehalfOfRoot {
        ModuleID requester;
        Identifier key;
        RefPtr<PlanOfLoad> plan;
        uint32_t depth;
    };
    Vector<FileAskedForOnBehalfOfRoot, 4> m_filesAskedForOnBehalfOfRoot;
    Vector<uint32_t> m_placesSeenByPlans;
    uint32_t m_firstPlaceOfNextPlan { 1 };
    // Their imported files ran without the loader. The set is filled from the list when it is asked.
    Vector<ModuleID, 4> m_rootsLoadedWithoutLoader;
    BitVector m_filesLoadedWithoutLoader;
    void noteModulesLoadedWith(ModuleID root);
    // A bit for each module, read for the heads of groups.
    Vector<uint64_t> m_loadedGroups;
    // The groups are those of a program in which the build has seen whoever names a module. Code from outside the image names what it likes: what is loaded from then on has a bit of its own.
    bool m_buildHasSeenWhoNamesModules;
    BitVector m_modulesLoadedBesideGroups;
    // A file ran before the loader came to it and queued jobs. They are queued where the loader comes to the file, as under `bun run`.
    std::optional<ModuleID> m_fileWhoseJobsWaitForLoader;
    std::optional<ModuleID> m_rootWhoseFilesRun;
    struct DeferredRequest {
        ModuleID file;
        Strong<JSCell> context;
        Strong<JSPromise> promise;
        bool useImportMap;
        Strong<JSCell> importerLoad { };
    };
    Vector<DeferredRequest> m_deferredRequests;
    struct DeferredImport {
        ModuleID file;
        Strong<JSPromise> promise;
    };
    Vector<DeferredImport> m_deferredImports;
    ModuleID fileOf(ModuleID fileOrFace) const { return m_table.fileBehindFace(fileOrFace).value_or(fileOrFace); }
    void forEachFileEvaluatedByLoader(const Invocable<void(ModuleID)> auto&) const;
    void forEachFileOrFaceEvaluatedByLoader(const Invocable<void(ModuleID)> auto&) const;
    bool isEvaluatedByLoader(ModuleID file) const;
    bool hasDeferredLoads(ModuleID fileOrFace) const;
    void resumeDeferredRequests(ModuleID file);
    void resumeDeferredImports(ModuleID file, uint8_t jobsUntilLoaded);
    void resumeDeferredLoadsWithoutImporter(ModuleID fileOrFace);
    void resumeDeferredLoadsOfFinishedFiles();
    bool hasKey(ModuleID file, const Identifier& key);
    std::optional<ModuleID> fileWithDeferredLoads(const Identifier& key);
    // The loader goes through its graph, and comes to files that have run.
    std::optional<ModuleID> m_rootWhoseFilesRanAhead;
    RefPtr<MicrotaskQueue> m_jobsThatWaitForLoader;
    // The queue that evaluateSynchronously() has opened for the files that run ahead. The collector finds it from the VM.
    VM::SynchronousModuleQueue* m_stepsThatWaitForLoader { nullptr };
    bool m_isStartingRuntimeModule { false };
    // The link of the entry point that the image has a plan for stores no status.
    bool m_entryPointIsLinked { false };
};

// What the host asks the engine for while the engine asks it for a module that the image names is no load by a name that the build did not follow.
class RequestToHostScope {
public:
    explicit RequestToHostScope(Modules& modules)
        : m_isAskingHost(modules.m_isAskingHost, true)
    {
    }

private:
    SetForScope<bool> m_isAskingHost;
};

Modules& modulesOf(Instance&);
Modules* modulesOf(JSGlobalObject*);

ALWAYS_INLINE StoredModuleStatus Modules::storedStatus(ModuleID module) const
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    return static_cast<StoredModuleStatus>(std::min(m_storedStatuses[module.index()], static_cast<uint8_t>(StoredModuleStatus::Evaluating)));
}

ALWAYS_INLINE void Modules::setStoredStatus(ModuleID module, StoredModuleStatus status)
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    m_storedStatuses[module.index()] = static_cast<uint8_t>(status);
}

ALWAYS_INLINE ModuleStatus Modules::status(ModuleID module) const
{
    switch (storedStatus(module)) {
    case StoredModuleStatus::Unlinked:
        return !data(module).needsLink() || (m_entryPointIsLinked && data(module).isLinkedWithEntryPoint()) ? ModuleStatus::Linked : ModuleStatus::Unlinked;
    case StoredModuleStatus::Linked:
        return ModuleStatus::Linked;
    case StoredModuleStatus::Evaluating:
        return ModuleStatus::Evaluating;
    case StoredModuleStatus::Evaluated:
        return ModuleStatus::Evaluated;
    case StoredModuleStatus::InRecord:
        return statusInRecord(module);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE auto Modules::loadedIfExists(ModuleID module) const -> LoadedModule*
{
    if (m_loadedFoundLast && m_moduleOfLoadedFoundLast == module)
        return m_loadedFoundLast;
    LoadedModule* loaded = m_loaded.get(module);
    if (loaded) {
        m_loadedFoundLast = loaded;
        m_moduleOfLoadedFoundLast = module;
    }
    return loaded;
}

ALWAYS_INLINE bool Modules::isLoadedModuleWithoutRecord(ModuleID module) const
{
    StoredModuleStatus stored = storedStatus(module);
    if (stored == StoredModuleStatus::Unlinked || stored == StoredModuleStatus::InRecord)
        return false;
    const ModuleData& moduleData = data(module);
    return moduleData.kind == ModuleKind::SourceText && !moduleData.isOfLibrary() && !moduleData.isFaceOfFile();
}

ALWAYS_INLINE bool Modules::isNotLoadedWithoutStatus(ModuleID module, const ModuleData& moduleData)
{
    if (m_entryPointIsLinked && moduleData.isLinkedWithEntryPoint())
        return false;
    if (moduleData.needsLink())
        return true;
    return !wasLoadedWithGraph(module);
}

// Of an ES module of the program. A graph is loaded before a module of it begins, and nothing is stored when a module that needs no link is loaded, nor when one with nothing to run is passed over.
ALWAYS_INLINE bool Modules::wasLoadedWithGraph(ModuleID module) const
{
    uint32_t head = m_table.headOfLoadGroup(module).index();
    if (m_loadedGroups[head / 64] >> (head % 64) & 1)
        return true;
    return !m_buildHasSeenWhoNamesModules && m_modulesLoadedBesideGroups.get(module.index());
}

ALWAYS_INLINE ModuleRecord* Modules::recordIfExists(ModuleID module) const
{
    return storedStatus(module) == StoredModuleStatus::InRecord ? m_records.get(module) : nullptr;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
